// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// af-broker - WebSocket connection support for agentflowd

package afbroker

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"strings"
	"time"
)

// WsClient is a WebSocket client for agentflowd
// Uses a raw TCP connection with HTTP upgrade for minimal dependencies
type WsClient struct {
	addr    string
	conn    net.Conn
	mu      chan struct{}
	idCount int64
}

// NewWsClient creates a WebSocket client for agentflowd
func NewWsClient(addr string) *WsClient {
	return &WsClient{
		addr: addr,
		mu:   make(chan struct{}, 1),
	}
}

// Connect performs the WebSocket handshake
func (w *WsClient) Connect(ctx context.Context) error {
	dialer := new(net.Dialer)
	conn, err := dialer.DialContext(ctx, "tcp", w.addr)
	if err != nil {
		return fmt.Errorf("ws connect: %w", err)
	}
	w.conn = conn

	// Send HTTP upgrade request
	host := w.addr
	if !strings.Contains(host, ":") {
		host = host + ":80"
	}
	upgradeReq := fmt.Sprintf(
		"GET / HTTP/1.1\r\n"+
			"Host: %s\r\n"+
			"Upgrade: websocket\r\n"+
			"Connection: Upgrade\r\n"+
			"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"+
			"Sec-WebSocket-Version: 13\r\n"+
			"\r\n", host)

	if _, err := conn.Write([]byte(upgradeReq)); err != nil {
		conn.Close()
		return fmt.Errorf("ws upgrade write: %w", err)
	}

	// Read response
	buf := make([]byte, 4096)
	conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	n, err := conn.Read(buf)
	if err != nil {
		conn.Close()
		return fmt.Errorf("ws upgrade read: %w", err)
	}
	conn.SetReadDeadline(time.Time{})

	response := string(buf[:n])
	if !strings.Contains(response, "101") || !strings.Contains(response, "Switching Protocols") {
		conn.Close()
		return errors.New("ws upgrade failed: server did not switch protocols")
	}

	return nil
}

// Close closes the WebSocket connection
func (w *WsClient) Close() error {
	if w.conn != nil {
		return w.conn.Close()
	}
	return nil
}

// WsCall sends a JSON-RPC request over WebSocket and reads the response
func (w *WsClient) WsCall(ctx context.Context, method string, params interface{}) (*Response, error) {
	w.mu <- struct{}{}
	defer func() { <-w.mu }()

	w.idCount++
	req := Request{
		JSONRPC: "2.0",
		Method:  method,
		Params:  params,
		ID:      w.idCount,
	}

	data, err := json.Marshal(req)
	if err != nil {
		return nil, fmt.Errorf("marshal: %w", err)
	}

	// Encode as WebSocket text frame
	frame := encodeWsFrame(data)
	if _, err := w.conn.Write(frame); err != nil {
		return nil, fmt.Errorf("ws write: %w", err)
	}

	// Read response frame
	if deadline, ok := ctx.Deadline(); ok {
		w.conn.SetReadDeadline(deadline)
	} else {
		w.conn.SetReadDeadline(time.Now().Add(30 * time.Second))
	}

	payload, err := readWsFrame(w.conn)
	w.conn.SetReadDeadline(time.Time{})
	if err != nil {
		return nil, fmt.Errorf("ws read: %w", err)
	}

	var resp Response
	if err := json.Unmarshal(payload, &resp); err != nil {
		return nil, fmt.Errorf("ws unmarshal: %w", err)
	}

	if resp.Error != nil {
		return &resp, resp.Error
	}
	return &resp, nil
}

// Minimal WebSocket frame encoding/decoding without external dependencies

func encodeWsFrame(payload []byte) []byte {
	frame := []byte{0x81} // FIN + text opcode

	length := len(payload)
	if length <= 125 {
		frame = append(frame, byte(length))
	} else if length <= 65535 {
		frame = append(frame, 126)
		frame = append(frame, byte(length>>8), byte(length))
	} else {
		frame = append(frame, 127)
		for i := 7; i >= 0; i-- {
			frame = append(frame, byte(length>>(i*8)))
		}
	}

	frame = append(frame, payload...)
	return frame
}

func readWsFrame(conn net.Conn) ([]byte, error) {
	header := make([]byte, 2)
	if _, err := readExact(conn, header); err != nil {
		return nil, err
	}

	opcode := header[0] & 0x0F
	_ = opcode
	masked := (header[1] & 0x80) != 0
	payloadLen := int(header[1] & 0x7F)

	var maskKey []byte

	if payloadLen == 126 {
		ext := make([]byte, 2)
		if _, err := readExact(conn, ext); err != nil {
			return nil, err
		}
		payloadLen = int(ext[0])<<8 | int(ext[1])
	} else if payloadLen == 127 {
		ext := make([]byte, 8)
		if _, err := readExact(conn, ext); err != nil {
			return nil, err
		}
		payloadLen = 0
		for i := 0; i < 8; i++ {
			payloadLen = payloadLen<<8 | int(ext[i])
		}
	}

	if masked {
		maskKey = make([]byte, 4)
		if _, err := readExact(conn, maskKey); err != nil {
			return nil, err
		}
	}

	payload := make([]byte, payloadLen)
	if _, err := readExact(conn, payload); err != nil {
		return nil, err
	}

	if masked && len(maskKey) == 4 {
		for i := range payload {
			payload[i] ^= maskKey[i%4]
		}
	}

	return payload, nil
}

func readExact(conn net.Conn, buf []byte) (int, error) {
	total := 0
	for total < len(buf) {
		n, err := conn.Read(buf[total:])
		if err != nil {
			return total, err
		}
		total += n
	}
	return total, nil
}

// VerifyWsClient satisfies the HTTP response check
var _ http.ResponseWriter = (*responseCheck)(nil)

type responseCheck struct{}

func (r *responseCheck) Header() http.Header           { return nil }
func (r *responseCheck) Write(b []byte) (int, error)   { return len(b), nil }
func (r *responseCheck) WriteHeader(statusCode int)     {}
