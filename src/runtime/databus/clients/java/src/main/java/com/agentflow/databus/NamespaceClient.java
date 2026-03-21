package com.agentflow.databus;

import java.util.HashMap;
import java.util.Map;
import java.util.function.BiConsumer;

/**
 * Namespace-scoped client for DataBus operations.
 *
 * <p>Provides chain-call access to a specific namespace (sys, ctx, llm, tool, user).
 * Automatically builds full keys with session and dialogue isolation.
 */
public class NamespaceClient {

    private final DataBusClient client;
    private final String namespace;
    private final String dialogueId;

    NamespaceClient(DataBusClient client, String namespace, String dialogueId) {
        this.client = client;
        this.namespace = namespace;
        this.dialogueId = dialogueId;
    }

    /**
     * Store a value in the DataBus.
     *
     * @param key   Key name
     * @param value Value to store (will be JSON-serialized)
     * @throws DataBusException If operation fails
     */
    public void put(String key, Object value) throws DataBusException {
        Map<String, Object> params = new HashMap<>();
        params.put("namespace", namespace);
        params.put("key", buildFullKey(key));
        params.put("value", value);
        client.call("put", params);
    }

    /**
     * Retrieve a value from the DataBus.
     *
     * @param key Key name
     * @return The stored value, or null if not found
     * @throws DataBusException If operation fails
     */
    public Object get(String key) throws DataBusException {
        Map<String, Object> params = new HashMap<>();
        params.put("namespace", namespace);
        params.put("key", buildFullKey(key));
        Map<String, Object> result = client.call("get", params);
        if (result != null && Boolean.TRUE.equals(result.get("exists"))) {
            return result.get("value");
        }
        return null;
    }

    /**
     * Retrieve a value with a default.
     *
     * @param key          Key name
     * @param defaultValue Default value if key not found
     * @return The stored value, or defaultValue if not found
     * @throws DataBusException If operation fails
     */
    public Object get(String key, Object defaultValue) throws DataBusException {
        Object value = get(key);
        return value != null ? value : defaultValue;
    }

    /**
     * Remove a key from the DataBus.
     *
     * @param key Key name
     * @return true if the key was removed
     * @throws DataBusException If operation fails
     */
    public boolean remove(String key) throws DataBusException {
        Map<String, Object> params = new HashMap<>();
        params.put("namespace", namespace);
        params.put("key", buildFullKey(key));
        Map<String, Object> result = client.call("remove", params);
        return result != null && Boolean.TRUE.equals(result.get("success"));
    }

    /**
     * Check if a key exists in the DataBus.
     *
     * @param key Key name
     * @return true if the key exists
     * @throws DataBusException If operation fails
     */
    public boolean contains(String key) throws DataBusException {
        Map<String, Object> params = new HashMap<>();
        params.put("namespace", namespace);
        params.put("key", buildFullKey(key));
        Map<String, Object> result = client.call("contains", params);
        return result != null && Boolean.TRUE.equals(result.get("exists"));
    }

    /**
     * Subscribe to key changes.
     *
     * @param key      Key name
     * @param callback Callback invoked when value changes
     * @return Subscription ID for later unsubscription
     * @throws DataBusException If operation fails
     */
    public long subscribe(String key, BiConsumer<String, Object> callback) throws DataBusException {
        Map<String, Object> params = new HashMap<>();
        params.put("namespace", namespace);
        params.put("key", buildFullKey(key));
        Map<String, Object> result = client.call("subscribe", params);
        if (result != null && result.get("subscription_id") != null) {
            long subscriptionId = ((Number) result.get("subscription_id")).longValue();
            // Note: Actual callback handling requires async notification support
            return subscriptionId;
        }
        throw new DataBusException("Failed to subscribe");
    }

    /**
     * Unsubscribe from key changes.
     *
     * @param subscriptionId Subscription ID returned from subscribe()
     * @return true if unsubscription succeeded
     * @throws DataBusException If operation fails
     */
    public boolean unsubscribe(long subscriptionId) throws DataBusException {
        Map<String, Object> params = new HashMap<>();
        params.put("subscription_id", subscriptionId);
        Map<String, Object> result = client.call("unsubscribe", params);
        return result != null && Boolean.TRUE.equals(result.get("success"));
    }

    /**
     * Publish an event to a topic.
     *
     * @param topic   Topic name
     * @param payload Event payload
     * @throws DataBusException If operation fails
     */
    public void publish(String topic, Object payload) throws DataBusException {
        Map<String, Object> params = new HashMap<>();
        params.put("namespace", namespace);
        params.put("topic", buildFullKey(topic));
        params.put("value", payload);
        client.call("publish", params);
    }

    /**
     * List keys in this namespace.
     *
     * @return Array of key names
     * @throws DataBusException If operation fails
     */
    public String[] listKeys() throws DataBusException {
        return listKeys(null);
    }

    /**
     * List keys with a prefix filter.
     *
     * @param prefix Key prefix to filter
     * @return Array of matching key names
     * @throws DataBusException If operation fails
     */
    @SuppressWarnings("unchecked")
    public String[] listKeys(String prefix) throws DataBusException {
        Map<String, Object> params = new HashMap<>();
        params.put("namespace", namespace);
        if (prefix != null && !prefix.isEmpty()) {
            params.put("prefix", prefix);
        }
        Map<String, Object> result = client.call("list_keys", params);
        if (result != null && result.get("keys") != null) {
            java.util.List<String> keys = (java.util.List<String>) result.get("keys");
            return keys.toArray(new String[0]);
        }
        return new String[0];
    }

    /**
     * Get the namespace name.
     *
     * @return Namespace name (sys, ctx, llm, tool, user)
     */
    public String getNamespace() {
        return namespace;
    }

    /**
     * Get the dialogue ID (if set).
     *
     * @return Dialogue ID or null
     */
    public String getDialogueId() {
        return dialogueId;
    }

    private String buildFullKey(String key) {
        if (dialogueId != null && !dialogueId.isEmpty()) {
            return dialogueId + "/" + key;
        }
        return key;
    }
}