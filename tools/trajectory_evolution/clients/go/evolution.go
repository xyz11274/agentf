// go 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

// 本包为 L0–L3 打点提供框架无关的 HTTP 和进程内传输。
package evolution

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"regexp"
	"strings"
	"sync"
	"time"
)

type Error struct {
	Code   string
	Status int
}

// 创建或读取受限错误信息，保留固定错误码而不泄露业务正文。
func (e *Error) Error() string { return e.Code }

type Options struct {
	Timeout          time.Duration
	MaxResponseBytes int64
	Retries          int
}
type Client struct {
	endpoint, token string
	http            *http.Client
	maxBytes        int64
	retries         int
	gaps            *pointGaps
}
type pointGaps struct {
	sync.Mutex
	runs map[string]bool
	full bool
}

// CallResult 保留原始业务值，同时暴露前后两个打点回执。
type CallResult struct {
	Output   any
	Before   map[string]any
	After    map[string]any
	Executed bool
}

// LoopDecision 包含循环守卫，以及上下文协议所需的提交回执。
type LoopDecision struct {
	Allow  bool
	Before map[string]any
	After  map[string]any
}

// 返回已保存的协议回执，不重新执行业务操作。
func (d LoopDecision) Receipt() map[string]any {
	if d.After != nil {
		return d.After
	}
	return d.Before
}

var safeCode = regexp.MustCompile(`^[a-z_]{1,96}$`)

// 验证参数并创建可复用客户端，在业务请求开始前初始化。
func New(endpoint, token string, options Options) (*Client, error) {
	if trialURL, trialToken := os.Getenv("TRAJECTORY_EVOLUTION_TRIAL_URL"), os.Getenv("TRAJECTORY_EVOLUTION_TRIAL_TOKEN"); trialURL != "" && trialToken != "" {
		endpoint, token = trialURL, trialToken
	}
	u, err := url.Parse(endpoint)
	if err != nil || u.Hostname() == "" || u.User != nil || u.RawQuery != "" || u.Fragment != "" || u.ForceQuery || (u.Path != "" && u.Path != "/") ||
		(u.Scheme != "https" && !(u.Scheme == "http" && (u.Hostname() == "localhost" || u.Hostname() == "127.0.0.1" || u.Hostname() == "::1"))) {
		return nil, &Error{Code: "https_or_loopback_url_required"}
	}
	if len(token) < 24 || strings.IndexFunc(token, func(c rune) bool { return c < 33 || c > 126 }) >= 0 {
		return nil, &Error{Code: "token_invalid"}
	}
	if options.Timeout == 0 {
		options.Timeout = 7 * time.Second
	}
	if options.MaxResponseBytes == 0 {
		options.MaxResponseBytes = 2097152
	}
	if options.Timeout < 100*time.Millisecond || options.Timeout > 300*time.Second || options.MaxResponseBytes < 1024 || options.MaxResponseBytes > 16777216 || options.Retries < 0 || options.Retries > 3 {
		return nil, &Error{Code: "client_limits_invalid"}
	}
	transport := http.DefaultTransport.(*http.Transport).Clone()
	transport.Proxy = nil
	transport.ForceAttemptHTTP2 = false
	return &Client{endpoint: u.Scheme + "://" + u.Host + "/v1/invoke", token: token, maxBytes: options.MaxResponseBytes, retries: options.Retries,
		gaps: &pointGaps{runs: make(map[string]bool)},
		http: &http.Client{Timeout: options.Timeout, Transport: transport, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}}, nil
}

// 安全关闭资源，等待在途调用结束并避免重复释放。
func (c *Client) Close() { c.http.CloseIdleConnections() }

// Evaluate 绑定现有接收入口；普通应用启动时不接管执行。
// 接收函数仅确认交付，真实结果由运行结束打点提供。
func Evaluate(ctx context.Context, receive func(context.Context, any) error) (bool, error) {
	if os.Getenv("TRAJECTORY_EVOLUTION_EVALUATION_ENTRY") != "1" {
		return false, nil
	}
	c, err := New(os.Getenv("TRAJECTORY_EVOLUTION_TRIAL_URL"), os.Getenv("TRAJECTORY_EVOLUTION_TRIAL_TOKEN"), Options{Timeout: 5 * time.Second})
	if err != nil {
		return true, &Error{Code: "evaluation_entry_failed"}
	}
	defer c.Close()
	var salt [16]byte
	if _, err = rand.Read(salt[:]); err != nil {
		return true, &Error{Code: "evaluation_entry_failed"}
	}
	consumer := hex.EncodeToString(salt[:])
	fail := func() (bool, error) {
		cleanup, cancel := context.WithTimeout(context.Background(), time.Second)
		defer cancel()
		_, _ = c.Invoke(cleanup, "evaluation.fail", map[string]any{"consumer": consumer}, "")
		return true, &Error{Code: "evaluation_entry_failed"}
	}
	for index := 0; ; {
		packet, err := c.Invoke(ctx, "evaluation.next", map[string]any{"consumer": consumer, "index": index}, "")
		if err != nil {
			return fail()
		}
		switch packet["state"] {
		case "done":
			return true, nil
		case "waiting":
			select {
			case <-ctx.Done():
				return fail()
			case <-time.After(100 * time.Millisecond):
			}
		case "input":
			if packet["index"] != float64(index) || receive(ctx, packet["input"]) != nil {
				return fail()
			}
			index++
		default:
			return fail()
		}
	}
}

// Point 使用同一总截止时间；观测端不可用不能变成 Agent 业务异常。
// 提交单个打点，保持策略准备在业务执行前、结果采集在执行后。
func (c *Client) Point(ctx context.Context, fields map[string]any, budget time.Duration) (result map[string]any) {
	ref, _ := fields["ref"].(map[string]any)
	run, _ := ref["run"].(string)
	if run != "" && len(run) <= 1024 {
		c.gaps.Lock()
		lost := c.gaps.full || c.gaps.runs[run]
		c.gaps.Unlock()
		if lost {
			packet := make(map[string]any, len(fields)+1)
			for key, value := range fields {
				packet[key] = value
			}
			packet["capture_gap"] = true
			fields = packet
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
	if budget == 0 {
		budget = 20 * time.Millisecond
	}
	if budget >= time.Millisecond && budget <= 5*time.Second {
		bounded, cancel := context.WithTimeout(ctx, budget)
		defer cancel()
		copyClient := *c
		copyClient.retries = 0
		if result, err := copyClient.Invoke(bounded, "point", fields, ""); err == nil {
			return result
		}
	}
	parent, _ := fields["context"].(map[string]any)
	context := make(map[string]any, len(parent)+3)
	for key, value := range parent {
		context[key] = value
	}
	kind, _ := fields["kind"].(string)
	if kind == "" {
		kind, _ = context["kind"].(string)
	}
	if kind == "" {
		kind = "custom"
	}
	context["kind"], context["degraded"], context["caused_by"] = kind, true, []any{}
	if _, exists := context["run_id"]; !exists {
		context["run_id"] = "unobserved"
	}
	decision := "allow"
	if kind == "loop" {
		decision = "continue"
	}
	return map[string]any{"context": context, "input": fields["input"], "decision": decision, "degraded": true,
		"capture_complete": false, "policy_applied": false, "closed": false, "ended": false, "reason": "observer_unavailable"}
}

// Call 包装一个现有 JSON 输入输出操作，execute 最多执行一次。
// 策略拒绝时跳过执行；业务异常保持原样返回。
// 最多执行一次宿主业务操作，策略拒绝时不调用业务回调。
func (c *Client) Call(ctx context.Context, fields map[string]any, execute func(any) (any, error), budget time.Duration) (CallResult, error) {
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

// CheckLoop 保留两种打点协议：引用式边界只执行一次守卫。
// 上下文式边界则以配对的 after 提交实际决定。
// 返回循环守卫与提交回执，保留两种打点协议。
func (c *Client) CheckLoop(ctx context.Context, fields map[string]any, budget time.Duration) LoopDecision {
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
		result.After = c.Point(ctx, map[string]any{"context": before["context"], "kind": "loop",
			"phase": "after", "decision": decision}, budget)
	}
	return result
}

// 取得并提交循环决定，返回是否允许继续。
func (c *Client) AllowLoop(ctx context.Context, fields map[string]any, budget time.Duration) bool {
	return c.CheckLoop(ctx, fields, budget).Allow
}

// 显式记录采集缺口，不将缺失轨迹作为完整样本。
func (c *Client) CaptureGap(ctx context.Context, fields map[string]any) (map[string]any, error) {
	return c.Invoke(ctx, "capture.gap", fields, "")
}

// 获取下一轮隔离评测输入，前一轮未收尾时不提前投递。
func (c *Client) EvaluationNext(ctx context.Context, consumer string, index int) (map[string]any, error) {
	return c.Invoke(ctx, "evaluation.next", map[string]any{"consumer": consumer, "index": index}, "")
}

// 报告评测入口失败，只传固定诊断，不传原始异常正文。
func (c *Client) EvaluationFail(ctx context.Context, consumer string) (map[string]any, error) {
	return c.Invoke(ctx, "evaluation.fail", map[string]any{"consumer": consumer}, "")
}

// 复制打点字段，避免补充协议数据时修改调用方对象。
func cloneFields(fields map[string]any) map[string]any {
	copy := make(map[string]any, len(fields)+1)
	for key, value := range fields {
		copy[key] = value
	}
	return copy
}

// Invoke 支持并发；传输结果不确定时复用 requestID，防止重复执行。
// 执行当前客户端协议请求，校验响应并保留请求身份。
func (c *Client) Invoke(ctx context.Context, operation string, fields map[string]any, requestID string) (map[string]any, error) {
	if requestID == "" {
		var id [16]byte
		if _, err := rand.Read(id[:]); err != nil {
			return nil, &Error{Code: "request_id_unavailable"}
		}
		requestID = hex.EncodeToString(id[:])
	}
	request := make(map[string]any, len(fields)+2)
	for key, value := range fields {
		request[key] = value
	}
	request["operation"], request["request_id"] = operation, requestID
	body, err := json.Marshal(request)
	if err != nil {
		return nil, &Error{Code: "request_invalid"}
	}
	if len(body) > 2097152 {
		return nil, &Error{Code: "request_too_large"}
	}
	for attempt := 0; ; attempt++ {
		result, failure := c.once(ctx, body)
		if failure == nil {
			return result, nil
		}
		if attempt >= c.retries || (failure.Status > 0 && failure.Status < 500) || ctx.Err() != nil {
			return nil, failure
		}
	}
}

// 执行一次传输尝试，后续重试只能由外层总预算和幂等规则决定。
func (c *Client) once(ctx context.Context, body []byte) (map[string]any, *Error) {
	request, err := http.NewRequestWithContext(ctx, "POST", c.endpoint, bytes.NewReader(body))
	if err != nil {
		return nil, &Error{Code: "request_invalid"}
	}
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Authorization", "Bearer "+c.token)
	response, err := c.http.Do(request)
	if err != nil {
		return nil, &Error{Code: "observer_unavailable"}
	}
	defer response.Body.Close()
	content, err := io.ReadAll(io.LimitReader(response.Body, c.maxBytes+1))
	if err != nil {
		return nil, &Error{Code: "observer_unavailable"}
	}
	if int64(len(content)) > c.maxBytes {
		return nil, &Error{Code: "response_too_large"}
	}
	var payload struct {
		OK     bool           `json:"ok"`
		Result map[string]any `json:"result"`
		Error  string         `json:"error"`
	}
	if json.Unmarshal(content, &payload) != nil {
		return nil, &Error{Code: "response_invalid", Status: response.StatusCode}
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 || !payload.OK {
		if !safeCode.MatchString(payload.Error) {
			payload.Error = "rpc_failed"
		}
		return nil, &Error{Code: payload.Error, Status: response.StatusCode}
	}
	if payload.Result == nil {
		return nil, &Error{Code: "response_invalid", Status: response.StatusCode}
	}
	return payload.Result, nil
}

// 客户端被打印用于诊断时，不能输出凭据。
// 返回安全对象描述，而非完整敏感配置。
func (c *Client) String() string { return "EvolutionClient" }

// 生成供调试打印的安全对象表示，不暴露凭据。
func (c *Client) GoString() string { return fmt.Sprint(c) }
