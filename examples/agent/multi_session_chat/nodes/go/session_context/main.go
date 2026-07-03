// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// session_context_nodes.go
// Go implementation of the four nodes in session-context-workflow:
//
//   read_prompt   — load the prompt template from disk
//   read_history  — read recent dialog history for the session
//   summarize     — call LLM to summarize the history
//   fill_context  — write the combined context to a session context file
//
// Usage: AF_* environment variables supply node inputs.
//        When AF_OUTPUT_FORMAT=json the node writes a JSON object to stdout so
//        that the framework's json_output protocol maps each field to the DataBus.
//        Otherwise a plain text status line is written (backward-compatible).
//
// The node to execute is selected by the first CLI argument, e.g.:
//   go run main.go read_prompt
//   go run main.go read_history
//   go run main.go summarize
//   go run main.go fill_context

package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// getenv returns the value of an AF_* prefixed environment variable,
// checking both the plain key and the AF_<UPPER_KEY> form.
func getenv(key string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	upper := "AF_" + strings.ToUpper(strings.ReplaceAll(key, "-", "_"))
	return os.Getenv(upper)
}

func packageRoot() string {
	if r := getenv("PACKAGE_ROOT"); r != "" {
		return r
	}
	return "examples/multi_session_chat/package"
}

func sessionID() string {
	if s := getenv("SESSION_ID"); s != "" {
		return s
	}
	return "precreated_session_1"
}

func readFile(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.TrimRight(string(data), "\r\n")
}

func writeFile(path, content string) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	return os.WriteFile(path, []byte(content), 0o644)
}

// jsonOutput writes outputs as a JSON object when AF_OUTPUT_FORMAT=json,
// otherwise falls back to printing a plain status line.
func jsonOutput(m map[string]string) {
	if os.Getenv("AF_OUTPUT_FORMAT") == "json" {
		data, _ := json.Marshal(m)
		fmt.Println(string(data))
		return
	}
	// Plain-text fallback: print the "status" key or first value
	if s, ok := m["status"]; ok {
		fmt.Println(s)
		return
	}
	for _, v := range m {
		fmt.Println(v)
		return
	}
}

// ---------------------------------------------------------------------------
// Node: read_prompt
// Loads the session summary prompt template.
// Output keys: prompt_template, status
// ---------------------------------------------------------------------------
func nodeReadPrompt() {
	root    := packageRoot()
	runtime := root + "/runtime"
	_ = os.MkdirAll(runtime, 0o755)

	promptPath := root + "/prompts/session_summary_prompt.txt"
	content    := readFile(promptPath)
	if content == "" {
		content = "Summarise the conversation history in 2-3 sentences."
	}

	// Mirror to runtime so downstream shell scripts can still find it
	_ = writeFile(runtime+"/prompt_loaded.txt", content)

	jsonOutput(map[string]string{
		"prompt_template": content,
		"status":          "prompt loaded",
	})
}

// ---------------------------------------------------------------------------
// Node: read_history
// Reads the most recent dialog history lines for the session.
// Output keys: history_tail, status
// ---------------------------------------------------------------------------
func nodeReadHistory() {
	root      := packageRoot()
	sessionID := sessionID()
	runtime   := root + "/runtime"
	_ = os.MkdirAll(runtime, 0o755)

	historyPath := root + "/history/" + sessionID + ".jsonl"
	f, err := os.Open(historyPath)
	if err != nil {
		// No history yet — write a placeholder
		placeholder := `{"role":"system","content":"empty history"}`
		_ = writeFile(root+"/history/"+sessionID+".jsonl", placeholder+"\n")
		_ = writeFile(runtime+"/history_tail.jsonl", placeholder+"\n")
		jsonOutput(map[string]string{
			"history_tail": placeholder,
			"status":       "history loaded (empty)",
		})
		return
	}
	defer f.Close()

	const maxLines = 30
	var lines []string
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		lines = append(lines, scanner.Text())
	}
	if len(lines) > maxLines {
		lines = lines[len(lines)-maxLines:]
	}
	tail := strings.Join(lines, "\n")
	_ = writeFile(runtime+"/history_tail.jsonl", tail+"\n")

	jsonOutput(map[string]string{
		"history_tail": tail,
		"status":       "history loaded",
	})
}

// ---------------------------------------------------------------------------
// Node: summarize
// Calls the LLM script to produce a session history summary.
// Reads prompt_template and history_tail from the DataBus (via AF_* env vars)
// or falls back to runtime files.
// Output keys: history_summary, status
// ---------------------------------------------------------------------------
func nodeSummarize() {
	root    := packageRoot()
	runtime := root + "/runtime"
	scripts := root + "/scripts"
	_ = os.MkdirAll(runtime, 0o755)

	prompt := getenv("PROMPT_TEMPLATE")
	if prompt == "" {
		prompt = readFile(runtime + "/prompt_loaded.txt")
	}
	if prompt == "" {
		prompt = readFile(root + "/prompts/session_summary_prompt.txt")
	}

	history := getenv("HISTORY_TAIL")
	if history == "" {
		history = readFile(runtime + "/history_tail.jsonl")
	}

	// Delegate to the LLM script
	summaryPath := runtime + "/history_summary.json"
	scriptCmd   := scripts + "/summarize_history.sh"
	if _, err := os.Stat(scriptCmd); err == nil {
		cmd := fmt.Sprintf("'%s' 2>/dev/null", scriptCmd)
		runShell(cmd)
	} else {
		// Direct LLM call if script is unavailable
		llmScript := scripts + "/llm_openai_compat.py"
		combinedPrompt := prompt + "\n" + history
		escapedPrompt := strings.ReplaceAll(combinedPrompt, "'", "'\\''")
		cmd := fmt.Sprintf("printf '%%s' '%s' | python3 '%s' > '%s' 2>/dev/null",
			escapedPrompt, llmScript, summaryPath)
		runShell(cmd)
	}

	summary := readFile(summaryPath)
	if summary == "" {
		summary = `{"summary":"No history available."}`
	}

	jsonOutput(map[string]string{
		"history_summary": summary,
		"status":          "history summarized",
	})
}

// ---------------------------------------------------------------------------
// Node: fill_context
// Merges summary and session metadata into the session context record.
// Output keys: session_context, status
// ---------------------------------------------------------------------------
func nodeFillContext() {
	root      := packageRoot()
	sessionID := sessionID()
	runtime   := root + "/runtime"
	_ = os.MkdirAll(runtime, 0o755)

	summary := getenv("HISTORY_SUMMARY")
	if summary == "" {
		summary = readFile(runtime + "/history_summary.json")
	}
	if summary == "" {
		summary = "{}"
	}

	ctx := fmt.Sprintf(`{"session_id":%q,"context":%s}`, sessionID, summary)
	ctxPath := runtime + "/" + sessionID + ".context.json"
	_ = writeFile(ctxPath, ctx)

	jsonOutput(map[string]string{
		"session_context": ctx,
		"status":          "context filled",
	})
}

// ---------------------------------------------------------------------------
// Shell helper
// ---------------------------------------------------------------------------
func runShell(cmd string) {
	c := exec.Command("/bin/sh", "-c", cmd)
	c.Stdout = os.Stdout
	c.Stderr = os.Stderr
	_ = c.Run()
}

// ---------------------------------------------------------------------------
// Main dispatcher
// ---------------------------------------------------------------------------
func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: session_context_nodes <node_name>")
		fmt.Fprintln(os.Stderr, "  node_name: read_prompt | read_history | summarize | fill_context")
		os.Exit(1)
	}
	switch os.Args[1] {
	case "read_prompt":
		nodeReadPrompt()
	case "read_history":
		nodeReadHistory()
	case "summarize":
		nodeSummarize()
	case "fill_context":
		nodeFillContext()
	default:
		fmt.Fprintf(os.Stderr, "unknown node: %s\n", os.Args[1])
		os.Exit(1)
	}
}
