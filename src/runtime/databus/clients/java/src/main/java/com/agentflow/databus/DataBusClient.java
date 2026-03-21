package com.agentflow.databus;

import java.io.IOException;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.ReentrantLock;
import java.util.function.BiConsumer;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

/**
 * DataBus client for multi-language access via Unix Domain Socket.
 *
 * <p>Provides real-time DataBus access with session/dialogue isolation.
 * Supports chain-call API matching the C++ INodeDataBusClient interface.
 *
 * <p>Usage:
 * <pre>
 * // Connect via environment variable
 * DataBusClient client = DataBusClient.fromEnv();
 *
 * // Or explicit socket path
 * DataBusClient client = new DataBusClient("/tmp/agentflow_session_xxx.sock");
 *
 * // Chain-call style
 * client.user().put("my_key", "value");
 * String value = client.user().get("my_key");
 *
 * // With dialogue_id
 * client.user("dialogue-123").put("result", "ok");
 *
 * // Close when done
 * client.close();
 * </pre>
 */
public class DataBusClient implements AutoCloseable {

    private final String socketPath;
    private final String sessionId;
    private final SocketChannel channel;
    private final ReentrantLock lock = new ReentrantLock();
    private final AtomicLong requestId = new AtomicLong(0);
    private final Gson gson = new GsonBuilder().create();

    // Cached namespace clients
    private NamespaceClient sysClient;
    private NamespaceClient ctxClient;
    private NamespaceClient llmClient;
    private NamespaceClient toolClient;
    private NamespaceClient userClient;
    private RawClient rawClient;

    /**
     * Create a new DataBus client.
     *
     * @param socketPath Path to the Unix Domain Socket
     * @throws IOException If connection fails
     */
    public DataBusClient(String socketPath) throws IOException {
        this.socketPath = socketPath;
        this.sessionId = extractSessionId(socketPath);

        UnixDomainSocketAddress address = UnixDomainSocketAddress.of(socketPath);
        this.channel = SocketChannel.open(StandardProtocolFamily.UNIX);
        this.channel.connect(address);
    }

    /**
     * Create a client using the AF_DATABUS_UDS_FILE environment variable.
     *
     * @return DataBusClient instance
     * @throws IOException If connection fails or environment variable not set
     */
    public static DataBusClient fromEnv() throws IOException {
        String socketPath = System.getenv("AF_DATABUS_UDS_FILE");
        if (socketPath == null || socketPath.isEmpty()) {
            throw new IOException("AF_DATABUS_UDS_FILE environment variable not set");
        }
        return new DataBusClient(socketPath);
    }

    private static String extractSessionId(String socketPath) {
        String basename = socketPath;
        int lastSlash = socketPath.lastIndexOf('/');
        if (lastSlash >= 0) {
            basename = socketPath.substring(lastSlash + 1);
        }
        if (basename.startsWith("agentflow_") && basename.endsWith(".sock")) {
            return basename.substring(10, basename.length() - 5);
        }
        return "";
    }

    /**
     * Get the session ID.
     * @return Session ID
     */
    public String getSessionId() {
        return sessionId;
    }

    /**
     * Get the socket path.
     * @return Socket path
     */
    public String getSocketPath() {
        return socketPath;
    }

    /**
     * Close the connection.
     * @throws IOException If closing fails
     */
    @Override
    public void close() throws IOException {
        if (channel != null && channel.isOpen()) {
            channel.close();
        }
    }

    /**
     * Access sys namespace (framework communication).
     * @return NamespaceClient for sys namespace
     */
    public NamespaceClient sys() {
        if (sysClient == null) {
            sysClient = new NamespaceClient(this, "sys", null);
        }
        return sysClient;
    }

    /**
     * Access sys namespace with dialogue ID.
     * @param dialogueId Dialogue ID
     * @return NamespaceClient for sys namespace
     */
    public NamespaceClient sys(String dialogueId) {
        return new NamespaceClient(this, "sys", dialogueId);
    }

    /**
     * Access ctx namespace (context storage).
     * @return NamespaceClient for ctx namespace
     */
    public NamespaceClient ctx() {
        if (ctxClient == null) {
            ctxClient = new NamespaceClient(this, "ctx", null);
        }
        return ctxClient;
    }

    /**
     * Access ctx namespace with dialogue ID.
     * @param dialogueId Dialogue ID
     * @return NamespaceClient for ctx namespace
     */
    public NamespaceClient ctx(String dialogueId) {
        return new NamespaceClient(this, "ctx", dialogueId);
    }

    /**
     * Access llm namespace (LLM service interaction).
     * @return NamespaceClient for llm namespace
     */
    public NamespaceClient llm() {
        if (llmClient == null) {
            llmClient = new NamespaceClient(this, "llm", null);
        }
        return llmClient;
    }

    /**
     * Access llm namespace with dialogue ID.
     * @param dialogueId Dialogue ID
     * @return NamespaceClient for llm namespace
     */
    public NamespaceClient llm(String dialogueId) {
        return new NamespaceClient(this, "llm", dialogueId);
    }

    /**
     * Access tool namespace (tool call information).
     * @return NamespaceClient for tool namespace
     */
    public NamespaceClient tool() {
        if (toolClient == null) {
            toolClient = new NamespaceClient(this, "tool", null);
        }
        return toolClient;
    }

    /**
     * Access tool namespace with dialogue ID.
     * @param dialogueId Dialogue ID
     * @return NamespaceClient for tool namespace
     */
    public NamespaceClient tool(String dialogueId) {
        return new NamespaceClient(this, "tool", dialogueId);
    }

    /**
     * Access user namespace (user-defined data).
     * @return NamespaceClient for user namespace
     */
    public NamespaceClient user() {
        if (userClient == null) {
            userClient = new NamespaceClient(this, "user", null);
        }
        return userClient;
    }

    /**
     * Access user namespace with dialogue ID.
     * @param dialogueId Dialogue ID
     * @return NamespaceClient for user namespace
     */
    public NamespaceClient user(String dialogueId) {
        return new NamespaceClient(this, "user", dialogueId);
    }

    /**
     * Get raw DataBus access (bypasses session isolation).
     * @return RawClient instance
     */
    public RawClient raw() {
        if (rawClient == null) {
            rawClient = new RawClient(this);
        }
        return rawClient;
    }

    @SuppressWarnings("unchecked")
    Map<String, Object> call(String method, Map<String, Object> params) throws DataBusException {
        lock.lock();
        try {
            long id = requestId.incrementAndGet();

            JsonObject request = new JsonObject();
            request.addProperty("jsonrpc", "2.0");
            request.addProperty("method", method);
            request.add("params", gson.toJsonTree(params));
            request.addProperty("id", id);

            // Send request
            String message = gson.toJson(request) + "\n";
            ByteBuffer buffer = StandardCharsets.UTF_8.encode(message);
            while (buffer.hasRemaining()) {
                channel.write(buffer);
            }

            // Receive response
            ByteBuffer readBuffer = ByteBuffer.allocate(8192);
            StringBuilder responseBuilder = new StringBuilder();
            while (true) {
                int bytesRead = channel.read(readBuffer);
                if (bytesRead < 0) {
                    throw new DataBusException("Connection closed");
                }
                readBuffer.flip();
                responseBuilder.append(StandardCharsets.UTF_8.decode(readBuffer));
                readBuffer.clear();

                if (responseBuilder.toString().contains("\n")) {
                    break;
                }
            }

            String responseStr = responseBuilder.toString().trim();
            JsonObject response = JsonParser.parseString(responseStr).getAsJsonObject();

            // Check for error
            if (response.has("error")) {
                JsonObject error = response.getAsJsonObject("error");
                int code = error.has("code") ? error.get("code").getAsInt() : -1;
                String msg = error.has("message") ? error.get("message").getAsString() : "Unknown error";
                throw new DataBusException(code, msg);
            }

            if (response.has("result")) {
                return gson.fromJson(response.get("result"), Map.class);
            }

            throw new DataBusException("Invalid response from DataBus");
        } catch (IOException e) {
            throw new DataBusException("IO error: " + e.getMessage(), e);
        } finally {
            lock.unlock();
        }
    }
}