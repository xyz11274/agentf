// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// af-broker - Go client adapter for agentflowd JSON-RPC 2.0

package afbroker

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"sync"
	"sync/atomic"
	"time"
)

// JSON-RPC 2.0 types

type Request struct {
	JSONRPC string      `json:"jsonrpc"`
	Method  string      `json:"method"`
	Params  interface{} `json:"params,omitempty"`
	ID      int64       `json:"id"`
}

type Response struct {
	JSONRPC string          `json:"jsonrpc"`
	Result  json.RawMessage `json:"result,omitempty"`
	Error   *RPCError       `json:"error,omitempty"`
	ID      int64           `json:"id"`
}

type RPCError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

func (e *RPCError) Error() string {
	return fmt.Sprintf("RPC error %d: %s", e.Code, e.Message)
}

type Notification struct {
	JSONRPC string          `json:"jsonrpc"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params,omitempty"`
}

// ChannelType determines which UDS socket to connect to
type ChannelType int

const (
	ChannelTCP   ChannelType = iota // TCP connection (all methods)
	ChannelBiz                      // UDS business channel
	ChannelCtrl                     // UDS control channel
)

// Client is a Go client for agentflowd JSON-RPC 2.0
type Client struct {
	addr        string
	channelType ChannelType
	conn        net.Conn
	reader      *bufio.Reader
	mu          sync.Mutex
	idCounter   atomic.Int64

	// Notification handler
	notifMu    sync.RWMutex
	notifHandler func(notification *Notification)
}

// NewClient creates a new agentflowd client
func NewClient(addr string, channelType ChannelType) *Client {
	return &Client{
		addr:        addr,
		channelType: channelType,
	}
}

// Connect establishes a connection to agentflowd
func (c *Client) Connect(ctx context.Context) error {
	var err error
	switch c.channelType {
	case ChannelTCP:
		c.conn, err = new(net.Dialer).DialContext(ctx, "tcp", c.addr)
	default:
		c.conn, err = net.DialTimeout("unix", c.addr, 5*time.Second)
	}
	if err != nil {
		return fmt.Errorf("connect: %w", err)
	}
	c.reader = bufio.NewReader(c.conn)

	// Start notification reader
	go c.readLoop()

	return nil
}

// Close closes the connection
func (c *Client) Close() error {
	if c.conn != nil {
		return c.conn.Close()
	}
	return nil
}

// SetNotificationHandler sets a callback for JSON-RPC notifications
func (c *Client) SetNotificationHandler(handler func(notification *Notification)) {
	c.notifMu.Lock()
	defer c.notifMu.Unlock()
	c.notifHandler = handler
}

// Call sends a JSON-RPC request and waits for the response
func (c *Client) Call(ctx context.Context, method string, params interface{}) (*Response, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	id := c.idCounter.Add(1)
	req := Request{
		JSONRPC: "2.0",
		Method:  method,
		Params:  params,
		ID:      id,
	}

	data, err := json.Marshal(req)
	if err != nil {
		return nil, fmt.Errorf("marshal request: %w", err)
	}
	data = append(data, '\n')

	// Set write deadline from context
	if deadline, ok := ctx.Deadline(); ok {
		c.conn.SetWriteDeadline(deadline)
	} else {
		c.conn.SetWriteDeadline(time.Now().Add(30 * time.Second))
	}

	if _, err := c.conn.Write(data); err != nil {
		return nil, fmt.Errorf("write: %w", err)
	}

	// For simplicity, we read the response inline.
	// In production, use a response channel and match by ID.
	// This works for sequential calls.
	if deadline, ok := ctx.Deadline(); ok {
		c.conn.SetReadDeadline(deadline)
	} else {
		c.conn.SetReadDeadline(time.Now().Add(30 * time.Second))
	}

	line, err := c.reader.ReadBytes('\n')
	if err != nil {
		return nil, fmt.Errorf("read: %w", err)
	}

	var resp Response
	if err := json.Unmarshal(line, &resp); err != nil {
		return nil, fmt.Errorf("unmarshal response: %w", err)
	}

	if resp.Error != nil {
		return &resp, resp.Error
	}

	return &resp, nil
}

func (c *Client) readLoop() {
	for {
		line, err := c.reader.ReadBytes('\n')
		if err != nil {
			return
		}

		// Try to parse as notification (no "id" field)
		var notif Notification
		if err := json.Unmarshal(line, &notif); err == nil && notif.Method != "" {
			c.notifMu.RLock()
			handler := c.notifHandler
			c.notifMu.RUnlock()
			if handler != nil {
				handler(&notif)
			}
		}
	}
}

// High-level API methods

// Health checks daemon health
func (c *Client) Health(ctx context.Context) (status string, uptimeMs int64, err error) {
	resp, err := c.Call(ctx, "health", nil)
	if err != nil {
		return "", 0, err
	}
	var result struct {
		Status   string `json:"status"`
		UptimeMs int64  `json:"uptime_ms"`
	}
	if err := json.Unmarshal(resp.Result, &result); err != nil {
		return "", 0, err
	}
	return result.Status, result.UptimeMs, nil
}

// AgentLoad loads an agent package
func (c *Client) AgentLoad(ctx context.Context, packagePath string) (agentID string, err error) {
	resp, err := c.Call(ctx, "agent.load", map[string]string{"package_path": packagePath})
	if err != nil {
		return "", err
	}
	var result struct {
		AgentID string `json:"agent_id"`
		Success bool   `json:"success"`
	}
	if err := json.Unmarshal(resp.Result, &result); err != nil {
		return "", err
	}
	if !result.Success {
		return "", errors.New("agent load failed")
	}
	return result.AgentID, nil
}

// AgentUnload unloads an agent
func (c *Client) AgentUnload(ctx context.Context, agentID string) error {
	_, err := c.Call(ctx, "agent.unload", map[string]string{"agent_id": agentID})
	return err
}

// AgentExecute sends a request to an agent
func (c *Client) AgentExecute(ctx context.Context, agentID, userID, clientID, content string) (messageID string, err error) {
	resp, err := c.Call(ctx, "agent.execute", map[string]string{
		"agent_id":  agentID,
		"user_id":   userID,
		"client_id": clientID,
		"content":   content,
	})
	if err != nil {
		return "", err
	}
	var result struct {
		MessageID string `json:"message_id"`
		Status    string `json:"status"`
	}
	if err := json.Unmarshal(resp.Result, &result); err != nil {
		return "", err
	}
	return result.MessageID, nil
}

// AgentPollResponse polls for agent response messages
func (c *Client) AgentPollResponse(ctx context.Context, agentID string, timeoutMs int) (*Response, error) {
	return c.Call(ctx, "agent.poll_response", map[string]interface{}{
		"agent_id":   agentID,
		"timeout_ms": timeoutMs,
	})
}

// EventSubscribe subscribes to event notifications
func (c *Client) EventSubscribe(ctx context.Context, eventTypes []string) (subscriptionID int, err error) {
	resp, err := c.Call(ctx, "event.subscribe", map[string]interface{}{
		"event_types": eventTypes,
	})
	if err != nil {
		return 0, err
	}
	var result struct {
		SubscriptionID int `json:"subscription_id"`
	}
	if err := json.Unmarshal(resp.Result, &result); err != nil {
		return 0, err
	}
	return result.SubscriptionID, nil
}

// EventUnsubscribe removes an event subscription
func (c *Client) EventUnsubscribe(ctx context.Context, subscriptionID int) error {
	_, err := c.Call(ctx, "event.unsubscribe", map[string]int{"subscription_id": subscriptionID})
	return err
}

// MetricsGet retrieves daemon metrics
func (c *Client) MetricsGet(ctx context.Context, format string) (*Response, error) {
	return c.Call(ctx, "metrics.get", map[string]string{"format": format})
}

// RuntimeShutdown requests graceful shutdown
func (c *Client) RuntimeShutdown(ctx context.Context) error {
	_, err := c.Call(ctx, "runtime.shutdown", nil)
	return err
}

// UpgradeToWebSocket attempts HTTP WebSocket upgrade on the TCP connection
func (c *Client) UpgradeToWebSocket(host string) (*http.Response, error) {
	// Not implemented in P5 - placeholder for future WebSocket support
	return nil, errors.New("WebSocket not yet implemented")
}
