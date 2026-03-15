package com.agentflow.databus;

import java.util.HashMap;
import java.util.Map;

/**
 * Raw DataBus client for direct key access.
 *
 * <p>Bypasses session isolation and allows direct key manipulation.
 * Use with caution - keys must be fully qualified.
 */
public class RawClient {

    private final DataBusClient client;

    RawClient(DataBusClient client) {
        this.client = client;
    }

    /**
     * Store a value with a raw key path.
     *
     * @param key   Full key path (e.g., "/session/abc/user/my_data")
     * @param value Value to store
     * @throws DataBusException If operation fails
     */
    public void put(String key, Object value) throws DataBusException {
        Map<String, Object> params = new HashMap<>();
        params.put("key", key);
        params.put("value", value);
        client.call("put", params);
    }

    /**
     * Retrieve a value with a raw key path.
     *
     * @param key Full key path
     * @return The stored value, or null if not found
     * @throws DataBusException If operation fails
     */
    public Object get(String key) throws DataBusException {
        Map<String, Object> params = new HashMap<>();
        params.put("key", key);
        Map<String, Object> result = client.call("get", params);
        if (result != null && Boolean.TRUE.equals(result.get("exists"))) {
            return result.get("value");
        }
        return null;
    }

    /**
     * Remove a key with a raw key path.
     *
     * @param key Full key path
     * @return true if the key was removed
     * @throws DataBusException If operation fails
     */
    public boolean remove(String key) throws DataBusException {
        Map<String, Object> params = new HashMap<>();
        params.put("key", key);
        Map<String, Object> result = client.call("remove", params);
        return result != null && Boolean.TRUE.equals(result.get("success"));
    }

    /**
     * Check if a key exists with a raw key path.
     *
     * @param key Full key path
     * @return true if the key exists
     * @throws DataBusException If operation fails
     */
    public boolean contains(String key) throws DataBusException {
        Map<String, Object> params = new HashMap<>();
        params.put("key", key);
        Map<String, Object> result = client.call("contains", params);
        return result != null && Boolean.TRUE.equals(result.get("exists"));
    }
}