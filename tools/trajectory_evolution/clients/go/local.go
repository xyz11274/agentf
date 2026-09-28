// go 客户端：通过统一协议收集执行事件并接收策略建议。
// 原生入口复用包内 C ABI；注意句柄生命周期与跨语言内存释放。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

package evolution

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"os"
	"strings"
	"sync"
	"time"
)

// LocalOptions 配置进程内运行时；LibraryPath 只在启动时解析。
// 留空时优先读取 TRAJECTORY_EVOLUTION_LOCAL_LIBRARY 环境配置，
// 其次按操作系统默认规则寻找 libtrajectory_evolution_local.so。
type LocalOptions struct {
	LibraryPath        string
	MaxRequestBytes    int
	MaxBufferBytes     int
	MaxPendingRuns     int
	PollIntervalMS     int
	Background         *bool
	ContentionBudgetUS int
}

// LocalEvolution 是稳定 te_local_* C 接口的类型化 Go 绑定。
// 它不发 HTTP、不创建子进程，可被多个协程共享。
type LocalEvolution struct {
	mu       sync.RWMutex
	native   localNative
	pid      int
	maxBytes int
	gaps     *pointGaps
}

type localNative interface {
	invoke([]byte) ([]byte, error)
	manage([]byte) ([]byte, error)
	metrics() ([]byte, error)
	setEnabled(bool) error
	drain() error
	close()
}

// OpenLocal 在运行时加载动态库，并创建一个可复用的本地运行时。
// Go 编译产物不嵌入开发机的动态库路径。
func OpenLocal(configPath, bearer string, options LocalOptions) (*LocalEvolution, error) {
	if configPath == "" || bearer == "" || strings.IndexByte(configPath, 0) >= 0 || strings.IndexByte(bearer, 0) >= 0 {
		return nil, &Error{Code: "local_configuration_missing"}
	}
	library := options.LibraryPath
	if library == "" {
		library = os.Getenv("TRAJECTORY_EVOLUTION_LOCAL_LIBRARY")
	}
	if library == "" {
		library = "libtrajectory_evolution_local.so"
	}
	if strings.IndexByte(library, 0) >= 0 {
		return nil, &Error{Code: "local_library_unavailable"}
	}
	maxBytes := options.MaxRequestBytes
	if maxBytes == 0 {
		maxBytes = 65536
	}
	if maxBytes < 1024 || maxBytes > 16777216 || options.MaxBufferBytes < 0 || options.MaxPendingRuns < 0 ||
		options.PollIntervalMS < 0 || options.ContentionBudgetUS < 0 || options.ContentionBudgetUS > 10000 {
		return nil, &Error{Code: "local_limits_invalid"}
	}
	encodedOptions, err := localOptionsJSON(options)
	if err != nil {
		return nil, &Error{Code: "local_limits_invalid"}
	}
	native, err := openNativeLocal(library, configPath, bearer, encodedOptions)
	if err != nil {
		return nil, err
	}
	return &LocalEvolution{native: native, pid: os.Getpid(), maxBytes: maxBytes,
		gaps: &pointGaps{runs: make(map[string]bool)}}, nil
}

// 序列化本地运行时选项，字段遵守稳定 C 接口契约。
func localOptionsJSON(options LocalOptions) ([]byte, error) {
	fields := make(map[string]any, 6)
	if options.MaxRequestBytes != 0 {
		fields["max_request_bytes"] = options.MaxRequestBytes
	}
	if options.MaxBufferBytes != 0 {
		fields["max_buffer_bytes"] = options.MaxBufferBytes
	}
	if options.MaxPendingRuns != 0 {
		fields["max_pending_runs"] = options.MaxPendingRuns
	}
	if options.PollIntervalMS != 0 {
		fields["poll_interval_ms"] = options.PollIntervalMS
	}
	if options.Background != nil {
		fields["background"] = *options.Background
	}
	if options.ContentionBudgetUS != 0 {
		fields["contention_budget_us"] = options.ContentionBudgetUS
	}
	return json.Marshal(fields)
}

// Close 等待在途原生调用结束，关闭运行时后卸载动态库。
// 重复关闭也是安全的。
// 安全关闭资源，等待在途调用结束并避免重复释放。
func (c *LocalEvolution) Close() {
	if c == nil {
		return
	}
	c.mu.Lock()
	native := c.native
	c.native = nil
	c.mu.Unlock()
	if native != nil {
		native.close()
	}
}

// 构造标准协议请求，补齐操作名称与幂等身份。
func (c *LocalEvolution) request(ctx context.Context, operation string, fields map[string]any, requestID string, management bool) (map[string]any, error) {
	if ctx == nil {
		ctx = context.Background()
	}
	select {
	case <-ctx.Done():
		return nil, &Error{Code: "local_unavailable"}
	default:
	}
	if requestID == "" {
		var id [16]byte
		if _, err := rand.Read(id[:]); err != nil {
			return nil, &Error{Code: "request_id_unavailable"}
		}
		requestID = hex.EncodeToString(id[:])
	}
	packet := make(map[string]any, len(fields)+2)
	for key, value := range fields {
		packet[key] = value
	}
	packet["operation"], packet["request_id"] = operation, requestID
	body, err := json.Marshal(packet)
	if err != nil {
		return nil, &Error{Code: "request_invalid"}
	}
	if len(body) > c.maxBytes {
		return nil, &Error{Code: "local_request_too_large"}
	}
	c.mu.RLock()
	defer c.mu.RUnlock()
	if c.native == nil || c.pid != os.Getpid() {
		return nil, &Error{Code: "local_unavailable"}
	}
	var encoded []byte
	if management {
		encoded, err = c.native.manage(body)
	} else {
		encoded, err = c.native.invoke(body)
	}
	if err != nil {
		return nil, err
	}
	var envelope struct {
		OK     bool           `json:"ok"`
		Result map[string]any `json:"result"`
		Error  string         `json:"error"`
	}
	if json.Unmarshal(encoded, &envelope) != nil {
		return nil, &Error{Code: "response_invalid"}
	}
	if !envelope.OK {
		if !safeCode.MatchString(envelope.Error) {
			envelope.Error = "local_unavailable"
		}
		return nil, &Error{Code: envelope.Error}
	}
	if envelope.Result == nil {
		return nil, &Error{Code: "response_invalid"}
	}
	return envelope.Result, nil
}

// Invoke 通过进程内运行时执行 L0–L3 热路径操作。
// 执行当前客户端协议请求，校验响应并保留请求身份。
func (c *LocalEvolution) Invoke(ctx context.Context, operation string, fields map[string]any, requestID string) (map[string]any, error) {
	return c.request(ctx, operation, fields, requestID, false)
}

// Manage 用于阻塞式 agent_evaluation.* 管理操作，不应放在 Agent 请求路径。
// 与 Invoke 不同，它可能执行评测或存储工作。
// 执行后台评测管理操作，不能用于主 Agent 低延迟边界。
func (c *LocalEvolution) Manage(ctx context.Context, operation string, fields map[string]any, requestID string) (map[string]any, error) {
	return c.request(ctx, operation, fields, requestID, true)
}

// 为本地打点故障构造原输入与降级标记，保持业务继续。
func localPointFallback(fields map[string]any) map[string]any {
	parent, _ := fields["context"].(map[string]any)
	pointContext := make(map[string]any, len(parent)+3)
	for key, value := range parent {
		pointContext[key] = value
	}
	kind, _ := fields["kind"].(string)
	if kind == "" {
		kind, _ = pointContext["kind"].(string)
	}
	if kind == "" {
		kind = "custom"
	}
	pointContext["kind"], pointContext["degraded"], pointContext["caused_by"] = kind, true, []any{}
	if _, exists := pointContext["run_id"]; !exists {
		pointContext["run_id"] = "unobserved"
	}
	decision := "allow"
	if kind == "loop" {
		decision = "continue"
	}
	return map[string]any{"context": pointContext, "input": fields["input"], "decision": decision,
		"degraded": true, "capture_complete": false, "policy_applied": false, "closed": false,
		"ended": false, "reason": "local_unavailable"}
}

// 合并已有回执和降级状态，保留先前完整性缺口。
func mergeLocalFallback(fields, observed map[string]any) map[string]any {
	result := localPointFallback(fields)
	for key, value := range observed {
		result[key] = value
	}
	return result
}

// 以最小内容记录缺口，避免失败重试再次暴露原始载荷。
func (c *LocalEvolution) reportGap(ctx context.Context, fields map[string]any) {
	gap := map[string]any{"reason": "observer_unavailable"}
	if reference, ok := fields["ref"].(map[string]any); ok {
		gap["phase"], gap["kind"] = "gap", "run"
		gap["ref"] = map[string]any{"run": reference["run"]}
		_, _ = c.request(ctx, "point", gap, "", false)
		return
	}
	if runID, ok := fields["run_id"]; ok {
		gap["run_id"] = runID
	} else if pointContext, ok := fields["context"].(map[string]any); ok {
		gap["run_id"] = pointContext["run_id"]
	}
	_, _ = c.request(ctx, "capture.gap", gap, "", false)
}

// Point 在观察失败时保持业务放行；budget 参数保留客户端兼容性，
// 本地锁竞争使用独立的有界等待设置。
// 提交单个打点，保持策略准备在业务执行前、结果采集在执行后。
func (c *LocalEvolution) Point(ctx context.Context, fields map[string]any, budget time.Duration) (result map[string]any) {
	request := cloneFields(fields)
	ref, _ := request["ref"].(map[string]any)
	run, _ := ref["run"].(string)
	if run != "" && len(run) <= 256 {
		c.gaps.Lock()
		lost := c.gaps.full || c.gaps.runs[run]
		c.gaps.Unlock()
		if lost {
			request["capture_gap"] = true
		}
		defer func() {
			c.gaps.Lock()
			defer c.gaps.Unlock()
			if result["degraded"] == true {
				if len(c.gaps.runs) < 1024 {
					c.gaps.runs[run] = true
				} else {
					c.gaps.full = true
				}
			} else if result["closed"] == true || result["run_closed"] == true {
				delete(c.gaps.runs, run)
			}
		}()
	}
	observed, err := c.Invoke(ctx, "point", request, "")
	if err != nil {
		c.reportGap(ctx, request)
		return localPointFallback(request)
	}
	if observed["degraded"] == true {
		return mergeLocalFallback(request, observed)
	}
	return observed
}

// Call 最多执行一次业务函数，并原样返回业务错误。
// 观测失败只体现在回执中。
// 最多执行一次宿主业务操作，策略拒绝时不调用业务回调。
func (c *LocalEvolution) Call(ctx context.Context, fields map[string]any, execute func(any) (any, error), budget time.Duration) (CallResult, error) {
	beforeFields := cloneFields(fields)
	beforeFields["phase"] = "before"
	before := c.Point(ctx, beforeFields, budget)
	actual, exists := before["input"]
	if !exists {
		actual = fields["input"]
	}
	terminal := make(map[string]any, 7)
	if ref, ok := fields["ref"]; ok {
		terminal["ref"] = ref
	} else {
		terminal["context"] = before["context"]
	}
	kind, _ := fields["kind"].(string)
	if kind == "" {
		kind = "custom"
	}
	terminal["kind"] = kind
	if before["decision"] == "reject" {
		reason, _ := before["reason"].(string)
		if reason == "" {
			reason = "policy_rejected"
		}
		output := map[string]any{"status": "rejected", "reason": reason, "error": reason}
		terminal["phase"], terminal["executed"] = "after", false
		terminal["actual_input"], terminal["output"] = actual, output
		after := c.Point(ctx, terminal, budget)
		return CallResult{Output: output, Before: before, After: after, Executed: false}, nil
	}
	output, err := execute(actual)
	if err != nil {
		terminal["phase"], terminal["status"], terminal["output"] = "abort", "unknown", nil
		after := c.Point(ctx, terminal, budget)
		return CallResult{Before: before, After: after, Executed: true}, err
	}
	terminal["phase"], terminal["executed"] = "after", true
	terminal["actual_input"], terminal["output"] = actual, output
	after := c.Point(ctx, terminal, budget)
	return CallResult{Output: output, Before: before, After: after, Executed: true}, nil
}

// 返回循环守卫与提交回执，保留两种打点协议。
func (c *LocalEvolution) CheckLoop(ctx context.Context, fields map[string]any, budget time.Duration) LoopDecision {
	request := cloneFields(fields)
	request["kind"] = "loop"
	_, referenced := request["ref"]
	if referenced {
		request["phase"] = "guard"
	} else {
		request["phase"] = "before"
	}
	before := c.Point(ctx, request, budget)
	decision, _ := before["decision"].(string)
	if decision == "" {
		decision = "continue"
	}
	result := LoopDecision{Allow: decision != "stop", Before: before}
	if !referenced {
		result.After = c.Point(ctx, map[string]any{"context": before["context"], "kind": "loop", "phase": "after", "decision": decision}, budget)
	}
	return result
}

// 取得并提交循环决定，返回是否允许继续。
func (c *LocalEvolution) AllowLoop(ctx context.Context, fields map[string]any, budget time.Duration) bool {
	return c.CheckLoop(ctx, fields, budget).Allow
}

// 显式记录采集缺口，不将缺失轨迹作为完整样本。
func (c *LocalEvolution) CaptureGap(ctx context.Context, fields map[string]any) (map[string]any, error) {
	return c.Invoke(ctx, "capture.gap", fields, "")
}

// 读取组件运行指标，不触发业务执行或模型请求。
func (c *LocalEvolution) Metrics() (map[string]any, error) {
	c.mu.RLock()
	defer c.mu.RUnlock()
	if c.native == nil || c.pid != os.Getpid() {
		return nil, &Error{Code: "local_unavailable"}
	}
	encoded, err := c.native.metrics()
	if err != nil {
		return nil, err
	}
	result := make(map[string]any)
	if json.Unmarshal(encoded, &result) != nil {
		return nil, &Error{Code: "response_invalid"}
	}
	return result, nil
}

// 切换附加进化能力，宿主原执行流程仍可独立运行。
func (c *LocalEvolution) SetEnabled(enabled bool) error {
	c.mu.RLock()
	defer c.mu.RUnlock()
	if c.native == nil || c.pid != os.Getpid() {
		return &Error{Code: "local_unavailable"}
	}
	return c.native.setEnabled(enabled)
}

// Drain 是阻塞式维护或关闭操作，不是 Agent 业务打点。
// 等待已接受的缓冲工作完成，仅在维护或关闭阶段使用。
func (c *LocalEvolution) Drain() error {
	c.mu.RLock()
	defer c.mu.RUnlock()
	if c.native == nil || c.pid != os.Getpid() {
		return &Error{Code: "local_unavailable"}
	}
	return c.native.drain()
}

// 返回安全对象描述，而非完整敏感配置。
func (c *LocalEvolution) String() string { return "LocalEvolution" }

// 生成供调试打印的安全对象表示，不暴露凭据。
func (c *LocalEvolution) GoString() string { return c.String() }
