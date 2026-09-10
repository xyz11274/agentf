// Package evolution is a framework-neutral HTTP transport for every L0-L3 boundary.
package evolution

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"regexp"
	"strings"
	"sync"
	"time"
)

type Error struct {
	Code   string
	Status int
}

func (e *Error) Error() string { return e.Code }

type Options struct {
	Timeout          time.Duration
	MaxResponseBytes int64
	Retries          int
}
type Client struct {
	endpoint, token string
	http            *http.Client
	maxBytes        int64
	retries         int
	gaps            *pointGaps
}
type pointGaps struct {
	sync.Mutex
	runs map[string]bool
	full bool
}

var safeCode = regexp.MustCompile(`^[a-z_]{1,96}$`)

func New(endpoint, token string, options Options) (*Client, error) {
	if trialURL, trialToken := os.Getenv("TRAJECTORY_EVOLUTION_TRIAL_URL"), os.Getenv("TRAJECTORY_EVOLUTION_TRIAL_TOKEN"); trialURL != "" && trialToken != "" {
		endpoint, token = trialURL, trialToken
	}
	u, err := url.Parse(endpoint)
	if err != nil || u.Hostname() == "" || u.User != nil || u.RawQuery != "" || u.Fragment != "" || u.ForceQuery || (u.Path != "" && u.Path != "/") ||
		(u.Scheme != "https" && !(u.Scheme == "http" && (u.Hostname() == "localhost" || u.Hostname() == "127.0.0.1" || u.Hostname() == "::1"))) {
		return nil, &Error{Code: "https_or_loopback_url_required"}
	}
	if len(token) < 24 || strings.IndexFunc(token, func(c rune) bool { return c < 33 || c > 126 }) >= 0 {
		return nil, &Error{Code: "token_invalid"}
	}
	if options.Timeout == 0 {
		options.Timeout = 7 * time.Second
	}
	if options.MaxResponseBytes == 0 {
		options.MaxResponseBytes = 2097152
	}
	if options.Timeout < 100*time.Millisecond || options.Timeout > 300*time.Second || options.MaxResponseBytes < 1024 || options.MaxResponseBytes > 16777216 || options.Retries < 0 || options.Retries > 3 {
		return nil, &Error{Code: "client_limits_invalid"}
	}
	transport := http.DefaultTransport.(*http.Transport).Clone()
	transport.Proxy = nil
	transport.ForceAttemptHTTP2 = false
	return &Client{endpoint: u.Scheme + "://" + u.Host + "/v1/invoke", token: token, maxBytes: options.MaxResponseBytes, retries: options.Retries,
		gaps: &pointGaps{runs: make(map[string]bool)},
		http: &http.Client{Timeout: options.Timeout, Transport: transport, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}}, nil
}

func (c *Client) Close() { c.http.CloseIdleConnections() }

// Point uses one total deadline and never turns an observer outage into an Agent error.
func (c *Client) Point(ctx context.Context, fields map[string]any, budget time.Duration) (result map[string]any) {
	ref, _ := fields["ref"].(map[string]any)
	run, _ := ref["run"].(string)
	if run != "" && len(run) <= 1024 {
		c.gaps.Lock()
		lost := c.gaps.full || c.gaps.runs[run]
		c.gaps.Unlock()
		if lost {
			packet := make(map[string]any, len(fields)+1)
			for key, value := range fields {
				packet[key] = value
			}
			packet["capture_gap"] = true
			fields = packet
		}
		defer func() {
			c.gaps.Lock()
			defer c.gaps.Unlock()
			if result["degraded"] == true {
				if len(c.gaps.runs) < 1024 {
					c.gaps.runs[run] = true
				} else {
					c.gaps.full = true
				}
			} else if result["closed"] == true || result["run_closed"] == true {
				delete(c.gaps.runs, run)
			}
		}()
	}
	if budget == 0 {
		budget = 20 * time.Millisecond
	}
	if budget >= time.Millisecond && budget <= 5*time.Second {
		bounded, cancel := context.WithTimeout(ctx, budget)
		defer cancel()
		copyClient := *c
		copyClient.retries = 0
		if result, err := copyClient.Invoke(bounded, "point", fields, ""); err == nil {
			return result
		}
	}
	parent, _ := fields["context"].(map[string]any)
	context := make(map[string]any, len(parent)+3)
	for key, value := range parent {
		context[key] = value
	}
	kind, _ := fields["kind"].(string)
	if kind == "" {
		kind, _ = context["kind"].(string)
	}
	if kind == "" {
		kind = "custom"
	}
	context["kind"], context["degraded"], context["caused_by"] = kind, true, []any{}
	if _, exists := context["run_id"]; !exists {
		context["run_id"] = "unobserved"
	}
	decision := "allow"
	if kind == "loop" {
		decision = "continue"
	}
	return map[string]any{"context": context, "input": fields["input"], "decision": decision, "degraded": true,
		"capture_complete": false, "policy_applied": false, "closed": false, "ended": false, "reason": "observer_unavailable"}
}

// Invoke can run concurrently. Reuse requestID after ambiguous transport failures.
func (c *Client) Invoke(ctx context.Context, operation string, fields map[string]any, requestID string) (map[string]any, error) {
	if requestID == "" {
		var id [16]byte
		if _, err := rand.Read(id[:]); err != nil {
			return nil, &Error{Code: "request_id_unavailable"}
		}
		requestID = hex.EncodeToString(id[:])
	}
	request := make(map[string]any, len(fields)+2)
	for key, value := range fields {
		request[key] = value
	}
	request["operation"], request["request_id"] = operation, requestID
	body, err := json.Marshal(request)
	if err != nil {
		return nil, &Error{Code: "request_invalid"}
	}
	if len(body) > 2097152 {
		return nil, &Error{Code: "request_too_large"}
	}
	for attempt := 0; ; attempt++ {
		result, failure := c.once(ctx, body)
		if failure == nil {
			return result, nil
		}
		if attempt >= c.retries || (failure.Status > 0 && failure.Status < 500) || ctx.Err() != nil {
			return nil, failure
		}
	}
}

func (c *Client) once(ctx context.Context, body []byte) (map[string]any, *Error) {
	request, err := http.NewRequestWithContext(ctx, "POST", c.endpoint, bytes.NewReader(body))
	if err != nil {
		return nil, &Error{Code: "request_invalid"}
	}
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Authorization", "Bearer "+c.token)
	response, err := c.http.Do(request)
	if err != nil {
		return nil, &Error{Code: "observer_unavailable"}
	}
	defer response.Body.Close()
	content, err := io.ReadAll(io.LimitReader(response.Body, c.maxBytes+1))
	if err != nil {
		return nil, &Error{Code: "observer_unavailable"}
	}
	if int64(len(content)) > c.maxBytes {
		return nil, &Error{Code: "response_too_large"}
	}
	var payload struct {
		OK     bool           `json:"ok"`
		Result map[string]any `json:"result"`
		Error  string         `json:"error"`
	}
	if json.Unmarshal(content, &payload) != nil {
		return nil, &Error{Code: "response_invalid", Status: response.StatusCode}
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 || !payload.OK {
		if !safeCode.MatchString(payload.Error) {
			payload.Error = "rpc_failed"
		}
		return nil, &Error{Code: payload.Error, Status: response.StatusCode}
	}
	if payload.Result == nil {
		return nil, &Error{Code: "response_invalid", Status: response.StatusCode}
	}
	return payload.Result, nil
}

// Avoid leaking credentials when callers print a client for diagnostics.
func (c *Client) String() string   { return "EvolutionClient" }
func (c *Client) GoString() string { return fmt.Sprint(c) }
