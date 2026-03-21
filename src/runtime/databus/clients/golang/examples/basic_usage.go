// AgentFlow DataBus Go SDK 基础用法示例
//
// 演示通过 UDS 连接 DataBus，进行 put/get/contains/remove/subscribe 操作。
//
// 运行前提：AgentFlow Worker 已启动并设置 AF_DATABUS_UDS_FILE 环境变量。
// 也可以显式指定 socket 路径：
//
//	go run basic_usage.go /tmp/agentflow_session_xxx.sock

package main

import (
	"errors"
	"fmt"
	"os"
	"time"

	"github.com/agentflow/databus/clients/golang/databus"
)

func demoBasicOperations(client *databus.Client) {
	fmt.Println("\n=== 基础读写操作 ===")

	// 写入字符串值
	if err := client.User().Put("greeting", "Hello, AgentFlow!"); err != nil {
		fmt.Printf("put 失败: %v\n", err)
		return
	}
	fmt.Println("put greeting: Hello, AgentFlow!")

	// 读取值
	value, err := client.User().Get("greeting")
	if err != nil {
		fmt.Printf("get 失败: %v\n", err)
		return
	}
	fmt.Printf("get greeting: %v\n", value)

	// 写入复杂对象
	config := map[string]any{
		"debug": true,
		"level": 5,
		"tags":  []string{"agent", "databus"},
	}
	if err := client.User().Put("config", config); err != nil {
		fmt.Printf("put config 失败: %v\n", err)
		return
	}
	fmt.Printf("put config: %v\n", config)

	retrieved, _ := client.User().Get("config")
	fmt.Printf("get config: %v\n", retrieved)

	// 检查键是否存在
	exists, _ := client.User().Contains("greeting")
	fmt.Printf("contains greeting: %v\n", exists)

	notExists, _ := client.User().Contains("nonexistent_key")
	fmt.Printf("contains nonexistent_key: %v\n", notExists)

	// 删除键
	client.User().Remove("greeting")
	fmt.Println("remove greeting")

	afterRemove, _ := client.User().Contains("greeting")
	fmt.Printf("contains greeting after remove: %v\n", afterRemove)
}

func demoNamespaces(client *databus.Client) {
	fmt.Println("\n=== 命名空间访问 ===")

	// sys 命名空间：框架通信
	client.Sys().Put("response", "OK")
	response, _ := client.Sys().Get("response")
	fmt.Printf("sys.response = %v\n", response)

	// ctx 命名空间：上下文存储
	client.Ctx().Put("history", []string{"msg1", "msg2"})
	history, _ := client.Ctx().Get("history")
	fmt.Printf("ctx.history = %v\n", history)

	// user 命名空间：用户自定义数据
	client.User().Put("result", map[string]any{"status": "completed"})
	result, _ := client.User().Get("result")
	fmt.Printf("user.result = %v\n", result)
}

func demoDialogueIsolation(client *databus.Client) {
	fmt.Println("\n=== 对话隔离 ===")

	dialogueA := "dialogue-001"
	dialogueB := "dialogue-002"

	// 两个对话写入相同键名，数据互不干扰
	client.UserWithDialogue(dialogueA).Put("skill_result", "Result from dialogue A")
	client.UserWithDialogue(dialogueB).Put("skill_result", "Result from dialogue B")

	resultA, _ := client.UserWithDialogue(dialogueA).Get("skill_result")
	resultB, _ := client.UserWithDialogue(dialogueB).Get("skill_result")
	fmt.Printf("Dialogue A skill_result: %v\n", resultA)
	fmt.Printf("Dialogue B skill_result: %v\n", resultB)
}

func demoSubscription(client *databus.Client) {
	fmt.Println("\n=== 订阅变更 ===")

	received := 0

	subID, err := client.User().Subscribe("status", func(key string, value any) {
		fmt.Printf("  [订阅回调] %s = %v\n", key, value)
		received++
	})
	if err != nil {
		fmt.Printf("订阅失败: %v\n", err)
		return
	}
	fmt.Printf("订阅 status，subscription_id=%d\n", subID)

	// 写入变更，触发回调
	for _, state := range []string{"pending", "running", "completed"} {
		client.User().Put("status", state)
		time.Sleep(100 * time.Millisecond)
	}
	time.Sleep(200 * time.Millisecond)

	// 取消订阅
	client.User().Unsubscribe(subID)
	fmt.Printf("取消订阅，共收到 %d 次变更通知\n", received)
}

func main() {
	var client *databus.Client
	var err error

	if len(os.Args) > 1 {
		client, err = databus.NewClient(os.Args[1])
	} else {
		client, err = databus.NewClientFromEnv()
	}

	if err != nil {
		if errors.Is(err, databus.ErrNotConnected) {
			fmt.Fprintln(os.Stderr, "未连接到 DataBus，请确认 AF_DATABUS_UDS_FILE 环境变量已设置")
		} else {
			fmt.Fprintf(os.Stderr, "连接失败: %v\n", err)
		}
		os.Exit(1)
	}
	defer client.Close()

	fmt.Printf("连接到 DataBus: %s\n", client.SocketPath())
	fmt.Printf("Session ID: %s\n", client.SessionID())

	demoBasicOperations(client)
	demoNamespaces(client)
	demoDialogueIsolation(client)
	demoSubscription(client)

	fmt.Println("\n示例运行完毕。")
}
