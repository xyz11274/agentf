package evolution

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"reflect"
	"sync"
	"testing"
	"time"
)

func testClient(t *testing.T) (*Client, *[]map[string]any) {
	t.Helper()
	var lock sync.Mutex
	requests := make([]map[string]any, 0)
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		var packet map[string]any
		if request.Header.Get("Authorization") != "Bearer fixture-client-token-000001" ||
			json.NewDecoder(request.Body).Decode(&packet) != nil {
			response.WriteHeader(http.StatusBadRequest)
			return
		}
		lock.Lock()
		requests = append(requests, packet)
		lock.Unlock()
		result := map[string]any{"event_id": "receipt", "decision": "allow"}
		if packet["operation"] == "point" {
			result["context"] = map[string]any{"run_id": "run-1", "kind": packet["kind"]}
			result["input"] = packet["input"]
			if packet["input"] == "rewrite" {
				result["input"] = "rewritten"
			}
			if packet["input"] == "reject" {
				result["decision"], result["reason"] = "reject", "blocked"
			}
			if packet["kind"] == "loop" && packet["state"] == "stop" {
				result["decision"] = "stop"
			}
		} else {
			result["operation"] = packet["operation"]
		}
		response.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(response).Encode(map[string]any{"ok": true, "result": result})
	}))
	t.Cleanup(server.Close)
	client, err := New(server.URL, "fixture-client-token-000001", Options{Timeout: time.Second})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(client.Close)
	return client, &requests
}

func TestCallExecutesOnceAndExposesReceipts(t *testing.T) {
	client, _ := testClient(t)
	calls := 0
	result, err := client.Call(context.Background(), map[string]any{
		"kind": "action", "ref": map[string]any{"run": "r", "key": "k"}, "input": "rewrite",
	}, func(input any) (any, error) {
		calls++
		if input != "rewritten" {
			t.Fatalf("policy input was not applied: %v", input)
		}
		return "done", nil
	}, time.Second)
	if err != nil || calls != 1 || !result.Executed || result.Output != "done" ||
		result.Before["event_id"] == nil || result.After["event_id"] == nil {
		t.Fatalf("unexpected call result: %#v err=%v calls=%d", result, err, calls)
	}
}

func TestCallRejectsWithoutExecutingAndPreservesHostError(t *testing.T) {
	client, _ := testClient(t)
	calls := 0
	rejected, err := client.Call(context.Background(), map[string]any{
		"kind": "action", "ref": map[string]any{"run": "r", "key": "reject"}, "input": "reject",
	}, func(any) (any, error) {
		calls++
		return nil, nil
	}, time.Second)
	if err != nil || calls != 0 || rejected.Executed || rejected.After["event_id"] == nil {
		t.Fatalf("rejected call executed or lost its receipt: %#v err=%v calls=%d", rejected, err, calls)
	}
	hostFailure := errors.New("host failure")
	failed, err := client.Call(context.Background(), map[string]any{
		"kind": "action", "ref": map[string]any{"run": "r", "key": "failure"}, "input": "run",
	}, func(any) (any, error) {
		calls++
		return nil, hostFailure
	}, time.Second)
	if err != hostFailure || calls != 1 || !failed.Executed || failed.After["event_id"] == nil {
		t.Fatalf("host error changed or abort was not observed: %#v err=%v calls=%d", failed, err, calls)
	}
}

func TestLoopProtocolsAndExplicitOperations(t *testing.T) {
	client, requests := testClient(t)
	contextDecision := client.CheckLoop(context.Background(), map[string]any{"state": "stop"}, time.Second)
	if contextDecision.Allow || contextDecision.After == nil || !reflect.DeepEqual(contextDecision.Receipt(), contextDecision.After) {
		t.Fatalf("context loop was not committed: %#v", contextDecision)
	}
	referenced := client.CheckLoop(context.Background(), map[string]any{
		"ref": map[string]any{"run": "r", "key": "loop"}, "state": "stop",
	}, time.Second)
	if referenced.Allow || referenced.After != nil || !reflect.DeepEqual(referenced.Receipt(), referenced.Before) {
		t.Fatalf("referenced loop should use one guard: %#v", referenced)
	}
	if _, err := client.CaptureGap(context.Background(), map[string]any{"run_id": "r", "reason": "missing_boundary"}); err != nil {
		t.Fatal(err)
	}
	if _, err := client.EvaluationNext(context.Background(), "consumer", 2); err != nil {
		t.Fatal(err)
	}
	if _, err := client.EvaluationFail(context.Background(), "consumer"); err != nil {
		t.Fatal(err)
	}
	phases := make([]any, 0)
	for _, request := range *requests {
		if request["operation"] == "point" && request["kind"] == "loop" {
			phases = append(phases, request["phase"])
		}
	}
	expected := []any{"before", "after", "guard"}
	if len(phases) != len(expected) {
		t.Fatalf("unexpected loop phases: %#v", phases)
	}
	for index := range expected {
		if phases[index] != expected[index] {
			t.Fatalf("unexpected loop phases: %#v", phases)
		}
	}
}
