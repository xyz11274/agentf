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

// LocalOptions configures the in-process runtime. LibraryPath is resolved at
// startup only; an empty value uses TRAJECTORY_EVOLUTION_LOCAL_LIBRARY and then
// the operating system's normal libtrajectory_evolution_local.so search path.
type LocalOptions struct {
	LibraryPath        string
	MaxRequestBytes    int
	MaxBufferBytes     int
	MaxPendingRuns     int
	PollIntervalMS     int
	Background         *bool
	ContentionBudgetUS int
}

// LocalEvolution is a typed Go binding to the stable te_local_* C ABI. It does
// no HTTP or process spawning and may be shared by concurrent goroutines.
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

// OpenLocal loads the shared library at runtime and creates one reusable local
// runtime. No library path is embedded into the Go binary at build time.
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

// Close waits for in-flight native calls, closes the runtime, and unloads the
// shared library. It is safe to call more than once.
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

// Invoke calls any L0-L3 hot-path operation through the in-process runtime.
func (c *LocalEvolution) Invoke(ctx context.Context, operation string, fields map[string]any, requestID string) (map[string]any, error) {
	return c.request(ctx, operation, fields, requestID, false)
}

// Manage invokes blocking agent_evaluation.* operations. Keep this off Agent
// request paths; unlike Invoke, it may perform evaluation or storage work.
func (c *LocalEvolution) Manage(ctx context.Context, operation string, fields map[string]any, requestID string) (map[string]any, error) {
	return c.request(ctx, operation, fields, requestID, true)
}

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

func mergeLocalFallback(fields, observed map[string]any) map[string]any {
	result := localPointFallback(fields)
	for key, value := range observed {
		result[key] = value
	}
	return result
}

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

// Point is fail-open: observer faults never become Agent faults. budget is kept
// for source compatibility with Client; local contention has its own bound.
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

// Call executes the business function at most once and returns its original
// error unchanged. Observation failures are visible only in the receipts.
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

func (c *LocalEvolution) AllowLoop(ctx context.Context, fields map[string]any, budget time.Duration) bool {
	return c.CheckLoop(ctx, fields, budget).Allow
}

func (c *LocalEvolution) CaptureGap(ctx context.Context, fields map[string]any) (map[string]any, error) {
	return c.Invoke(ctx, "capture.gap", fields, "")
}

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

func (c *LocalEvolution) SetEnabled(enabled bool) error {
	c.mu.RLock()
	defer c.mu.RUnlock()
	if c.native == nil || c.pid != os.Getpid() {
		return &Error{Code: "local_unavailable"}
	}
	return c.native.setEnabled(enabled)
}

// Drain is a blocking maintenance/shutdown operation, not an Agent boundary.
func (c *LocalEvolution) Drain() error {
	c.mu.RLock()
	defer c.mu.RUnlock()
	if c.native == nil || c.pid != os.Getpid() {
		return &Error{Code: "local_unavailable"}
	}
	return c.native.drain()
}

func (c *LocalEvolution) String() string   { return "LocalEvolution" }
func (c *LocalEvolution) GoString() string { return c.String() }
