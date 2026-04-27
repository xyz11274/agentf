// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// af-broker - CLI tool for interacting with agentflowd

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"time"

	"github.com/agentflow/af-broker"
)

func main() {
	if len(os.Args) < 3 {
		fmt.Fprintln(os.Stderr, "Usage: af-broker <addr> <command> [args...]")
		fmt.Fprintln(os.Stderr, "")
		fmt.Fprintln(os.Stderr, "Commands:")
		fmt.Fprintln(os.Stderr, "  health                              Check daemon health")
		fmt.Fprintln(os.Stderr, "  load <package_path>                 Load an agent package")
		fmt.Fprintln(os.Stderr, "  unload <agent_id>                   Unload an agent")
		fmt.Fprintln(os.Stderr, "  list                                List loaded agents")
		fmt.Fprintln(os.Stderr, "  execute <agent_id> <content>        Execute a request")
		fmt.Fprintln(os.Stderr, "  poll <agent_id> [timeout_ms]        Poll for responses")
		fmt.Fprintln(os.Stderr, "  subscribe <event_type,...>          Subscribe to events")
		fmt.Fprintln(os.Stderr, "  metrics [json|prometheus]           Get metrics")
		fmt.Fprintln(os.Stderr, "  shutdown                            Shutdown daemon")
		os.Exit(1)
	}

	addr := os.Args[1]
	cmd := os.Args[2]

	client := afbroker.NewClient(addr, afbroker.ChannelTCP)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	if err := client.Connect(ctx); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to connect: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()

	// Set up notification handler
	client.SetNotificationHandler(func(n *afbroker.Notification) {
		data, _ := json.MarshalIndent(n, "", "  ")
		fmt.Printf("[NOTIFICATION] %s\n", string(data))
	})

	switch cmd {
	case "health":
		status, uptime, err := client.Health(ctx)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("Status: %s, Uptime: %dms\n", status, uptime)

	case "load":
		if len(os.Args) < 4 {
			fmt.Fprintln(os.Stderr, "Usage: af-broker <addr> load <package_path>")
			os.Exit(1)
		}
		agentID, err := client.AgentLoad(ctx, os.Args[3])
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("Agent loaded: %s\n", agentID)

	case "unload":
		if len(os.Args) < 4 {
			fmt.Fprintln(os.Stderr, "Usage: af-broker <addr> unload <agent_id>")
			os.Exit(1)
		}
		if err := client.AgentUnload(ctx, os.Args[3]); err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}
		fmt.Println("Agent unloaded")

	case "list":
		resp, err := client.Call(ctx, "agent.list", nil)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}
		var pretty json.RawMessage = resp.Result
		data, _ := json.MarshalIndent(pretty, "", "  ")
		fmt.Println(string(data))

	case "execute":
		if len(os.Args) < 5 {
			fmt.Fprintln(os.Stderr, "Usage: af-broker <addr> execute <agent_id> <content>")
			os.Exit(1)
		}
		msgID, err := client.AgentExecute(ctx, os.Args[3], "cli-user", "af-broker", os.Args[4])
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("Request submitted: %s\n", msgID)

	case "poll":
		if len(os.Args) < 4 {
			fmt.Fprintln(os.Stderr, "Usage: af-broker <addr> poll <agent_id> [timeout_ms]")
			os.Exit(1)
		}
		timeoutMs := 5000
		if len(os.Args) >= 5 {
			fmt.Sscanf(os.Args[4], "%d", &timeoutMs)
		}
		resp, err := client.AgentPollResponse(ctx, os.Args[3], timeoutMs)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}
		data, _ := json.MarshalIndent(resp.Result, "", "  ")
		fmt.Println(string(data))

	case "subscribe":
		if len(os.Args) < 4 {
			fmt.Fprintln(os.Stderr, "Usage: af-broker <addr> subscribe <event_type,...>")
			os.Exit(1)
		}
		// Parse comma-separated event types
		types := []string{}
		for _, t := range splitComma(os.Args[3]) {
			types = append(types, t)
		}
		subID, err := client.EventSubscribe(ctx, types)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("Subscribed (id=%d). Press Ctrl+C to stop.\n", subID)
		// Block until interrupted
		select {}

	case "metrics":
		format := "json"
		if len(os.Args) >= 4 {
			format = os.Args[3]
		}
		resp, err := client.MetricsGet(ctx, format)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}
		fmt.Println(string(resp.Result))

	case "shutdown":
		if err := client.RuntimeShutdown(ctx); err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}
		fmt.Println("Shutdown requested")

	default:
		fmt.Fprintf(os.Stderr, "Unknown command: %s\n", cmd)
		os.Exit(1)
	}
}

func splitComma(s string) []string {
	var result []string
	start := 0
	for i := 0; i <= len(s); i++ {
		if i == len(s) || s[i] == ',' {
			if i > start {
				result = append(result, s[start:i])
			}
			start = i + 1
		}
	}
	return result
}
