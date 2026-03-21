// Package databus provides a DataBus client for multi-language access via Unix Domain Socket.
//
// Provides real-time DataBus access with session/dialogue isolation.
// Supports chain-call API matching the C++ INodeDataBusClient interface.
//
// Usage:
//
//	client, err := databus.NewClientFromEnv()  // Uses AF_DATABUS_UDS_FILE
//	if err != nil {
//	    log.Fatal(err)
//	}
//	defer client.Close()
//
//	// Chain-call style (matches C++ API)
//	client.User().Put("my_key", "value")
//	value, err := client.User().Get("my_key")
//	client.Sys().Put("response", "Hello World")
//
//	// With dialogue_id
//	client.UserWithDialogue("dialogue-123").Put("skill_result", map[string]any{"status": "ok"})
//
//	// Subscription
//	subID, err := client.User().Subscribe("my_key", func(key string, value any) {
//	    fmt.Printf("Key %s changed to %v\n", key, value)
//	})
//	client.User().Unsubscribe(subID)
package databus

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
)

// Errors
var (
	ErrNotConnected    = errors.New("not connected to DataBus")
	ErrInvalidResponse = errors.New("invalid response from DataBus")
	ErrTimeout         = errors.New("operation timed out")
)

// Error represents a JSON-RPC error
type Error struct {
	Code    int
	Message string
}

func (e *Error) Error() string {
	return fmt.Sprintf("JSON-RPC Error %d: %s", e.Code, e.Message)
}

// NamespaceClient provides namespace-scoped DataBus access
type NamespaceClient struct {
	client     *Client
	namespace  string
	dialogueID string
	subs       map[int64]func(string, any)
	subsMu     sync.Mutex
}

// Put stores a value with the given key
func (n *NamespaceClient) Put(key string, value any) error {
	params := map[string]any{
		"session_id": n.client.sessionID,
		"namespace":  n.namespace,
		"key":        key,
		"value":      value,
	}
	result, err := n.client.call("put", params)
	if err != nil {
		return err
	}
	if success, ok := result["success"].(bool); ok && success {
		return nil
	}
	return errors.New("put operation failed")
}

// Get retrieves a value by key
func (n *NamespaceClient) Get(key string) (any, error) {
	params := map[string]any{
		"session_id": n.client.sessionID,
		"namespace":  n.namespace,
		"key":        key,
	}
	result, err := n.client.call("get", params)
	if err != nil {
		return nil, err
	}
	if exists, ok := result["exists"].(bool); ok && exists {
		return result["value"], nil
	}
	return nil, nil
}

// GetString retrieves a string value by key
func (n *NamespaceClient) GetString(key string) (string, error) {
	val, err := n.Get(key)
	if err != nil {
		return "", err
	}
	if str, ok := val.(string); ok {
		return str, nil
	}
	return "", nil
}

// GetInt retrieves an int64 value by key
func (n *NamespaceClient) GetInt(key string) (int64, error) {
	val, err := n.Get(key)
	if err != nil {
		return 0, err
	}
	if f, ok := val.(float64); ok {
		return int64(f), nil
	}
	return 0, nil
}

// GetFloat retrieves a float64 value by key
func (n *NamespaceClient) GetFloat(key string) (float64, error) {
	val, err := n.Get(key)
	if err != nil {
		return 0, err
	}
	if f, ok := val.(float64); ok {
		return f, nil
	}
	return 0, nil
}

// GetBool retrieves a bool value by key
func (n *NamespaceClient) GetBool(key string) (bool, error) {
	val, err := n.Get(key)
	if err != nil {
		return false, err
	}
	if b, ok := val.(bool); ok {
		return b, nil
	}
	return false, nil
}

// Remove removes a key-value pair
func (n *NamespaceClient) Remove(key string) error {
	params := map[string]any{
		"session_id": n.client.sessionID,
		"namespace":  n.namespace,
		"key":        key,
	}
	result, err := n.client.call("remove", params)
	if err != nil {
		return err
	}
	if success, ok := result["success"].(bool); ok && success {
		return nil
	}
	return errors.New("remove operation failed")
}

// Contains checks if a key exists
func (n *NamespaceClient) Contains(key string) (bool, error) {
	params := map[string]any{
		"session_id": n.client.sessionID,
		"namespace":  n.namespace,
		"key":        key,
	}
	result, err := n.client.call("contains", params)
	if err != nil {
		return false, err
	}
	if exists, ok := result["exists"].(bool); ok {
		return exists, nil
	}
	return false, nil
}

// Subscribe subscribes to data changes for a specific key
func (n *NamespaceClient) Subscribe(key string, callback func(string, any)) (int64, error) {
	params := map[string]any{
		"session_id": n.client.sessionID,
		"namespace":  n.namespace,
		"key":        key,
	}
	result, err := n.client.call("subscribe", params)
	if err != nil {
		return 0, err
	}
	var subID int64
	if id, ok := result["subscription_id"].(float64); ok {
		subID = int64(id)
	}

	n.subsMu.Lock()
	if n.subs == nil {
		n.subs = make(map[int64]func(string, any))
	}
	n.subs[subID] = callback
	n.subsMu.Unlock()

	return subID, nil
}

// Unsubscribe cancels a subscription
func (n *NamespaceClient) Unsubscribe(subscriptionID int64) error {
	params := map[string]any{
		"subscription_id": subscriptionID,
	}
	result, err := n.client.call("unsubscribe", params)
	if err != nil {
		return err
	}

	n.subsMu.Lock()
	delete(n.subs, subscriptionID)
	n.subsMu.Unlock()

	if success, ok := result["success"].(bool); ok && success {
		return nil
	}
	return errors.New("unsubscribe operation failed")
}

// Publish publishes an event to a topic
func (n *NamespaceClient) Publish(topic string, value any) error {
	params := map[string]any{
		"session_id": n.client.sessionID,
		"namespace":  n.namespace,
		"topic":      topic,
		"value":      value,
	}
	result, err := n.client.call("publish", params)
	if err != nil {
		return err
	}
	if success, ok := result["success"].(bool); ok && success {
		return nil
	}
	return errors.New("publish operation failed")
}

// RawClient provides raw DataBus access without namespace prefix
type RawClient struct {
	client *Client
}

// Put stores a value with the given key (raw, no prefix)
func (r *RawClient) Put(key string, value any) error {
	params := map[string]any{
		"session_id": "",
		"namespace":  "",
		"key":        strings.TrimPrefix(key, "/"),
		"value":      value,
	}
	result, err := r.client.call("put", params)
	if err != nil {
		return err
	}
	if success, ok := result["success"].(bool); ok && success {
		return nil
	}
	return errors.New("put operation failed")
}

// Get retrieves a value by key (raw, no prefix)
func (r *RawClient) Get(key string) (any, error) {
	params := map[string]any{
		"session_id": "",
		"namespace":  "",
		"key":        strings.TrimPrefix(key, "/"),
	}
	result, err := r.client.call("get", params)
	if err != nil {
		return nil, err
	}
	if exists, ok := result["exists"].(bool); ok && exists {
		return result["value"], nil
	}
	return nil, nil
}

// Contains checks if a key exists (raw, no prefix)
func (r *RawClient) Contains(key string) (bool, error) {
	params := map[string]any{
		"session_id": "",
		"namespace":  "",
		"key":        strings.TrimPrefix(key, "/"),
	}
	result, err := r.client.call("contains", params)
	if err != nil {
		return false, err
	}
	if exists, ok := result["exists"].(bool); ok {
		return exists, nil
	}
	return false, nil
}

// Remove removes a key-value pair (raw, no prefix)
func (r *RawClient) Remove(key string) error {
	params := map[string]any{
		"session_id": "",
		"namespace":  "",
		"key":        strings.TrimPrefix(key, "/"),
	}
	result, err := r.client.call("remove", params)
	if err != nil {
		return err
	}
	if success, ok := result["success"].(bool); ok && success {
		return nil
	}
	return errors.New("remove operation failed")
}

// Client is the main DataBus client
type Client struct {
	socketPath string
	sessionID  string
	conn       net.Conn
	reader     *bufio.Reader
	mu         sync.Mutex
	requestID  int64

	// Cached namespace clients
	sysClient  *NamespaceClient
	ctxClient  *NamespaceClient
	llmClient  *NamespaceClient
	toolClient *NamespaceClient
	userClient *NamespaceClient
	rawClient  *RawClient
}

// NewClient creates a new DataBus client
func NewClient(socketPath string) (*Client, error) {
	// Extract session ID from socket path
	sessionID := ""
	basename := socketPath
	if idx := strings.LastIndex(socketPath, "/"); idx >= 0 {
		basename = socketPath[idx+1:]
	}
	if strings.HasPrefix(basename, "agentflow_") && strings.HasSuffix(basename, ".sock") {
		sessionID = basename[10 : len(basename)-5]
	}

	client := &Client{
		socketPath: socketPath,
		sessionID:  sessionID,
	}

	if err := client.connect(); err != nil {
		return nil, err
	}

	return client, nil
}

// NewClientFromEnv creates a client using the AF_DATABUS_UDS_FILE environment variable
func NewClientFromEnv() (*Client, error) {
	socketPath := os.Getenv("AF_DATABUS_UDS_FILE")
	if socketPath == "" {
		return nil, errors.New("AF_DATABUS_UDS_FILE environment variable not set")
	}
	return NewClient(socketPath)
}

func (c *Client) connect() error {
	c.mu.Lock()
	defer c.mu.Unlock()

	conn, err := net.Dial("unix", c.socketPath)
	if err != nil {
		return fmt.Errorf("failed to connect to %s: %w", c.socketPath, err)
	}

	c.conn = conn
	c.reader = bufio.NewReader(conn)
	return nil
}

// Close closes the connection
func (c *Client) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.conn != nil {
		err := c.conn.Close()
		c.conn = nil
		c.reader = nil
		return err
	}
	return nil
}

// SessionID returns the session ID
func (c *Client) SessionID() string {
	return c.sessionID
}

// SocketPath returns the socket path
func (c *Client) SocketPath() string {
	return c.socketPath
}

func (c *Client) call(method string, params map[string]any) (map[string]any, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.conn == nil {
		return nil, ErrNotConnected
	}

	c.requestID++
	requestID := c.requestID

	request := map[string]any{
		"jsonrpc": "2.0",
		"method":  method,
		"params":  params,
		"id":      requestID,
	}

	// Send request
	requestJSON, err := json.Marshal(request)
	if err != nil {
		return nil, fmt.Errorf("failed to marshal request: %w", err)
	}
	requestJSON = append(requestJSON, '\n')

	if _, err := c.conn.Write(requestJSON); err != nil {
		return nil, fmt.Errorf("failed to send request: %w", err)
	}

	// Receive response
	responseLine, err := c.reader.ReadString('\n')
	if err != nil {
		return nil, fmt.Errorf("failed to receive response: %w", err)
	}

	var response map[string]any
	if err := json.Unmarshal([]byte(responseLine), &response); err != nil {
		return nil, fmt.Errorf("failed to unmarshal response: %w", err)
	}

	// Check for error
	if errObj, ok := response["error"].(map[string]any); ok {
		code := 0
		if c, ok := errObj["code"].(float64); ok {
			code = int(c)
		}
		msg := "unknown error"
		if m, ok := errObj["message"].(string); ok {
			msg = m
		}
		return nil, &Error{Code: code, Message: msg}
	}

	result, ok := response["result"].(map[string]any)
	if !ok {
		return nil, ErrInvalidResponse
	}

	return result, nil
}

// Sys returns a namespace client for the sys namespace
func (c *Client) Sys() *NamespaceClient {
	if c.sysClient == nil {
		c.sysClient = &NamespaceClient{client: c, namespace: "sys"}
	}
	return c.sysClient
}

// Ctx returns a namespace client for the ctx namespace
func (c *Client) Ctx() *NamespaceClient {
	if c.ctxClient == nil {
		c.ctxClient = &NamespaceClient{client: c, namespace: "ctx"}
	}
	return c.ctxClient
}

// LLM returns a namespace client for the llm namespace
func (c *Client) LLM() *NamespaceClient {
	if c.llmClient == nil {
		c.llmClient = &NamespaceClient{client: c, namespace: "llm"}
	}
	return c.llmClient
}

// Tool returns a namespace client for the tool namespace
func (c *Client) Tool() *NamespaceClient {
	if c.toolClient == nil {
		c.toolClient = &NamespaceClient{client: c, namespace: "tool"}
	}
	return c.toolClient
}

// User returns a namespace client for the user namespace
func (c *Client) User() *NamespaceClient {
	if c.userClient == nil {
		c.userClient = &NamespaceClient{client: c, namespace: "user"}
	}
	return c.userClient
}

// SysWithDialogue returns a namespace client for sys namespace with dialogue ID
func (c *Client) SysWithDialogue(dialogueID string) *NamespaceClient {
	return &NamespaceClient{client: c, namespace: "sys", dialogueID: dialogueID}
}

// CtxWithDialogue returns a namespace client for ctx namespace with dialogue ID
func (c *Client) CtxWithDialogue(dialogueID string) *NamespaceClient {
	return &NamespaceClient{client: c, namespace: "ctx", dialogueID: dialogueID}
}

// LLMWithDialogue returns a namespace client for llm namespace with dialogue ID
func (c *Client) LLMWithDialogue(dialogueID string) *NamespaceClient {
	return &NamespaceClient{client: c, namespace: "llm", dialogueID: dialogueID}
}

// ToolWithDialogue returns a namespace client for tool namespace with dialogue ID
func (c *Client) ToolWithDialogue(dialogueID string) *NamespaceClient {
	return &NamespaceClient{client: c, namespace: "tool", dialogueID: dialogueID}
}

// UserWithDialogue returns a namespace client for user namespace with dialogue ID
func (c *Client) UserWithDialogue(dialogueID string) *NamespaceClient {
	return &NamespaceClient{client: c, namespace: "user", dialogueID: dialogueID}
}

// Raw returns a raw client without namespace prefix
func (c *Client) Raw() *RawClient {
	if c.rawClient == nil {
		c.rawClient = &RawClient{client: c}
	}
	return c.rawClient
}

// Helper function to convert string to int64
func parseInt(s string) int64 {
	i, _ := strconv.ParseInt(s, 10, 64)
	return i
}