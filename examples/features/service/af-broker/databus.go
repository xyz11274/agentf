// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// af-broker - DataBus operations for agentflowd

package afbroker

import (
	"context"
	"encoding/json"
	"fmt"
)

// DataBusPut stores a key-value pair in the agent's DataBus
func (c *Client) DataBusPut(ctx context.Context, agentID, sessionID, key, value string) error {
	params := map[string]string{
		"agent_id":   agentID,
		"session_id": sessionID,
		"key":        key,
		"value":      value,
	}
	_, err := c.Call(ctx, "databus.put", params)
	return err
}

// DataBusGet retrieves a value from the agent's DataBus
func (c *Client) DataBusGet(ctx context.Context, agentID, sessionID, key string) (string, error) {
	params := map[string]string{
		"agent_id":   agentID,
		"session_id": sessionID,
		"key":        key,
	}
	resp, err := c.Call(ctx, "databus.get", params)
	if err != nil {
		return "", err
	}
	var result struct {
		Value string `json:"value"`
	}
	if err := json.Unmarshal(resp.Result, &result); err != nil {
		return "", fmt.Errorf("unmarshal databus.get result: %w", err)
	}
	return result.Value, nil
}

// DataBusRemove removes a key from the agent's DataBus
func (c *Client) DataBusRemove(ctx context.Context, agentID, sessionID, key string) error {
	params := map[string]string{
		"agent_id":   agentID,
		"session_id": sessionID,
		"key":        key,
	}
	_, err := c.Call(ctx, "databus.remove", params)
	return err
}

// DataBusContains checks if a key exists in the agent's DataBus
func (c *Client) DataBusContains(ctx context.Context, agentID, sessionID, key string) (bool, error) {
	params := map[string]string{
		"agent_id":   agentID,
		"session_id": sessionID,
		"key":        key,
	}
	resp, err := c.Call(ctx, "databus.contains", params)
	if err != nil {
		return false, err
	}
	var result struct {
		Exists bool `json:"exists"`
	}
	if err := json.Unmarshal(resp.Result, &result); err != nil {
		return false, fmt.Errorf("unmarshal databus.contains result: %w", err)
	}
	return result.Exists, nil
}

// DataBusListKeys lists all keys in the agent's DataBus namespace
func (c *Client) DataBusListKeys(ctx context.Context, agentID, sessionID string) ([]string, error) {
	params := map[string]string{
		"agent_id":   agentID,
		"session_id": sessionID,
	}
	resp, err := c.Call(ctx, "databus.list_keys", params)
	if err != nil {
		return nil, err
	}
	var result struct {
		Keys []string `json:"keys"`
	}
	if err := json.Unmarshal(resp.Result, &result); err != nil {
		return nil, fmt.Errorf("unmarshal databus.list_keys result: %w", err)
	}
	return result.Keys, nil
}
