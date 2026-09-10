package main

import (
	"bufio"
	"context"
	"encoding/json"
	evolution "example.org/trajectory-evolution-client"
	"os"
)

func main() {
	client, err := evolution.New(os.Getenv("EVOLUTION_URL"), os.Getenv("EVOLUTION_TEST_TOKEN"), evolution.Options{})
	if err != nil {
		panic("client_initialization_failed")
	}
	defer client.Close()
	input := bufio.NewScanner(os.Stdin)
	input.Buffer(make([]byte, 65536), 2097152)
	output := json.NewEncoder(os.Stdout)
	for input.Scan() {
		var request map[string]any
		if json.Unmarshal(input.Bytes(), &request) != nil {
			panic("probe_input_invalid")
		}
		operation, id := request["operation"].(string), request["request_id"].(string)
		delete(request, "operation")
		delete(request, "request_id")
		result, err := client.Invoke(context.Background(), operation, request, id)
		if err != nil {
			failure := err.(*evolution.Error)
			output.Encode(map[string]any{"ok": false, "error": failure.Code, "status": failure.Status})
		} else {
			output.Encode(map[string]any{"ok": true, "result": result})
		}
	}
	if input.Err() != nil {
		panic("probe_read_failed")
	}
}
