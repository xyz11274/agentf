// go 客户端：通过统一协议收集执行事件并接收策略建议。
// 原生入口复用包内 C ABI；注意句柄生命周期与跨语言内存释放。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。
// 本文件包含协议验证或接入示例，不作为真实业务效果的评测成绩。

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

// 执行一次组件协议调用，检查响应信封并保持请求身份一致。
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

// 执行评测等阻塞式管理操作，与主 Agent 热路径分离。
func (f *fakeLocalNative) manage(body []byte) ([]byte, error) { return f.invoke(body) }

// 读取组件自身的采集和排队指标，不把观测指标当作业务任务得分。
func (f *fakeLocalNative) metrics() ([]byte, error) { return []byte(`{"degraded_calls":0}`), nil }

// 切换组件附加能力，不更改宿主原有业务和安全逻辑。
func (f *fakeLocalNative) setEnabled(bool) error { return nil }

// 等待当前待持久化工作完成，用于维护或关闭；不要放在主 Agent 每个调用的热路径。
func (f *fakeLocalNative) drain() error { return nil }

// 关闭资源并等待本实现要求的在途操作完成，重复关闭按协议处理。
func (f *fakeLocalNative) close() { f.mu.Lock(); f.closed = true; f.mu.Unlock() }

// 测试辅助函数：localWithFake。
// 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
func localWithFake(native *fakeLocalNative) *LocalEvolution {
	return &LocalEvolution{native: native, pid: currentPID(), maxBytes: 65536,
		gaps: &pointGaps{runs: make(map[string]bool)}}
}

// 读取当前进程标识，用于测试进程归属与生命周期。
func currentPID() int { return os.Getpid() }

// 回归用例：TestLocalCallExecutesBusinessOnceAndPreservesFailure。
// 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
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

// 回归用例：TestLocalPointFailsOpenAndManageUsesManagementPath。
// 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
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

// 回归用例：TestLocalCloseWaitsForInflightInvoke。
// 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
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
