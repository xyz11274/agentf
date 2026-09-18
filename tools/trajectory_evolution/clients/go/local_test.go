package evolution

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"sync"
	"testing"
	"time"
)

type fakeLocalNative struct {
	mu       sync.Mutex
	requests []map[string]any
	fail     bool
	closed   bool
	entered  chan struct{}
	release  chan struct{}
}

func (f *fakeLocalNative) invoke(body []byte) ([]byte, error) {
	if f.entered != nil {
		close(f.entered)
		<-f.release
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.fail {
		return nil, errors.New("offline")
	}
	var request map[string]any
	if json.Unmarshal(body, &request) != nil {
		return nil, errors.New("invalid")
	}
	f.requests = append(f.requests, request)
	result := map[string]any{"event_id": "receipt", "decision": "allow"}
	if request["operation"] == "point" {
		result["context"] = map[string]any{"run_id": "run", "kind": request["kind"]}
		result["input"] = request["input"]
	}
	return json.Marshal(map[string]any{"ok": true, "result": result})
}
func (f *fakeLocalNative) manage(body []byte) ([]byte, error) { return f.invoke(body) }
func (f *fakeLocalNative) metrics() ([]byte, error)           { return []byte(`{"degraded_calls":0}`), nil }
func (f *fakeLocalNative) setEnabled(bool) error              { return nil }
func (f *fakeLocalNative) drain() error                       { return nil }
func (f *fakeLocalNative) close()                             { f.mu.Lock(); f.closed = true; f.mu.Unlock() }

func localWithFake(native *fakeLocalNative) *LocalEvolution {
	return &LocalEvolution{native: native, pid: currentPID(), maxBytes: 65536,
		gaps: &pointGaps{runs: make(map[string]bool)}}
}

func currentPID() int { return os.Getpid() }

func TestLocalCallExecutesBusinessOnceAndPreservesFailure(t *testing.T) {
	native := &fakeLocalNative{}
	client := localWithFake(native)
	calls := 0
	result, err := client.Call(context.Background(), map[string]any{
		"kind": "action", "ref": map[string]any{"run": "r", "key": "one"}, "input": "payload",
	}, func(input any) (any, error) {
		calls++
		return map[string]any{"input": input}, nil
	}, 0)
	if err != nil || calls != 1 || !result.Executed || result.After["event_id"] == nil {
		t.Fatalf("unexpected local call: %#v err=%v calls=%d", result, err, calls)
	}
	business := errors.New("business")
	failed, err := client.Call(context.Background(), map[string]any{
		"kind": "action", "ref": map[string]any{"run": "r", "key": "two"}, "input": "payload",
	}, func(any) (any, error) {
		calls++
		return nil, business
	}, 0)
	if err != business || calls != 2 || !failed.Executed || failed.After["event_id"] == nil {
		t.Fatalf("business error changed or abort receipt missing: %#v err=%v", failed, err)
	}
}

func TestLocalPointFailsOpenAndManageUsesManagementPath(t *testing.T) {
	native := &fakeLocalNative{fail: true}
	client := localWithFake(native)
	point := client.Point(context.Background(), map[string]any{
		"kind": "loop", "ref": map[string]any{"run": "r", "key": "loop"}, "state": "same",
	}, 0)
	if point["degraded"] != true || point["decision"] != "continue" {
		t.Fatalf("observer failure did not fail open: %#v", point)
	}
	native.fail = false
	if _, err := client.Manage(context.Background(), "agent_evaluation.status", nil, "management-id"); err != nil {
		t.Fatal(err)
	}
	native.mu.Lock()
	defer native.mu.Unlock()
	if got := native.requests[len(native.requests)-1]["operation"]; got != "agent_evaluation.status" {
		t.Fatalf("unexpected management operation: %v", got)
	}
}

func TestLocalCloseWaitsForInflightInvoke(t *testing.T) {
	native := &fakeLocalNative{entered: make(chan struct{}), release: make(chan struct{})}
	client := localWithFake(native)
	invoked := make(chan struct{})
	go func() {
		_, _ = client.Invoke(context.Background(), "contract.describe", nil, "")
		close(invoked)
	}()
	<-native.entered
	closed := make(chan struct{})
	go func() { client.Close(); close(closed) }()
	select {
	case <-closed:
		t.Fatal("close raced past an in-flight native call")
	case <-time.After(20 * time.Millisecond):
	}
	close(native.release)
	select {
	case <-invoked:
	case <-time.After(time.Second):
		t.Fatal("invoke did not finish")
	}
	select {
	case <-closed:
	case <-time.After(time.Second):
		t.Fatal("close did not finish")
	}
	native.mu.Lock()
	defer native.mu.Unlock()
	if !native.closed {
		t.Fatal("native runtime was not closed")
	}
}
