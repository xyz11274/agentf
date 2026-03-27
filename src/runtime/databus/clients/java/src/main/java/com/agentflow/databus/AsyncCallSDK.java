package com.agentflow.databus;

import java.util.Map;
import java.util.HashMap;

/**
 * Async call SDK for multi-language nodes using DataBus UDS channel.
 *
 * <p>Provides async callback support for Python/Shell/Go nodes
 * to send messages back to the runtime without blocking.
 *
 * <p>Environment variables required:
 * <ul>
 *   <li>AF_SESSION_ID: Current session ID</li>
 *   <li>AF_DIALOGUE_ID: Current dialogue ID</li>
 *   <li>AF_SELF_TASK_ID: Current task ID</li>
 *   <li>AF_NODE_ID: Current node ID</li>
 * </ul>
 *
 * <p>Usage:
 * <pre>
 * AsyncCallSDK asyncSDK = client.asyncSDK();
 *
 * // Send message to current task
 * boolean success = asyncSDK.sendToSelf("callback_event", "{\"status\": \"ok\"}");
 *
 * // Send message to another task
 * boolean success = asyncSDK.sendMsg("task-456", "node-789", "event", "content");
 * </pre>
 */
public class AsyncCallSDK {

    private final DataBusClient client;
    private final String sessionId;
    private final String dialogueId;
    private final String taskId;
    private final String nodeId;

    /**
     * Create an AsyncCallSDK instance.
     *
     * @param client The parent DataBusClient
     */
    public AsyncCallSDK(DataBusClient client) {
        this.client = client;
        this.sessionId = System.getenv().getOrDefault("AF_SESSION_ID", "");
        this.dialogueId = System.getenv().getOrDefault("AF_DIALOGUE_ID", "");
        this.taskId = System.getenv().getOrDefault("AF_SELF_TASK_ID", "");
        this.nodeId = System.getenv().getOrDefault("AF_NODE_ID", "");
    }

    /**
     * Get the session ID.
     * @return Session ID
     */
    public String getSessionId() {
        return sessionId;
    }

    /**
     * Get the dialogue ID.
     * @return Dialogue ID
     */
    public String getDialogueId() {
        return dialogueId;
    }

    /**
     * Get the task ID.
     * @return Task ID
     */
    public String getTaskId() {
        return taskId;
    }

    /**
     * Get the node ID.
     * @return Node ID
     */
    public String getNodeId() {
        return nodeId;
    }

    /**
     * Send an async message to the current task (self).
     *
     * <p>This enables short-circuit routing directly to the task queue,
     * bypassing the agent routing layer for lower latency.
     *
     * @param callKey The call key for message routing
     * @param content The message content
     * @return true if the message was successfully enqueued
     * @throws DataBusException If the operation fails
     */
    public boolean sendToSelf(String callKey, String content) throws DataBusException {
        Map<String, Object> params = new HashMap<>();
        params.put("session_id", sessionId);
        params.put("dialogue_id", dialogueId);
        params.put("task_id", taskId);
        params.put("node_id", nodeId);
        params.put("call_key", callKey);
        params.put("content", content);

        Map<String, Object> result = client.call("async_callback", params);
        if (result.containsKey("success")) {
            return Boolean.TRUE.equals(result.get("success"));
        }
        return false;
    }

    /**
     * Send an async message to a different task/node.
     *
     * @param targetTaskId Target task ID
     * @param targetNodeId Target node ID
     * @param callKey The call key for message routing
     * @param content The message content
     * @return true if the message was successfully enqueued
     * @throws DataBusException If the operation fails
     */
    public boolean sendMsg(String targetTaskId, String targetNodeId, String callKey, String content)
            throws DataBusException {
        Map<String, Object> params = new HashMap<>();
        params.put("session_id", sessionId);
        params.put("dialogue_id", dialogueId);
        params.put("task_id", targetTaskId);
        params.put("node_id", targetNodeId);
        params.put("call_key", callKey);
        params.put("content", content);

        Map<String, Object> result = client.call("async_callback", params);
        if (result.containsKey("success")) {
            return Boolean.TRUE.equals(result.get("success"));
        }
        return false;
    }
}