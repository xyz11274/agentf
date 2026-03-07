package agentflow.sdk;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Base64;

public final class AgentFlowDataBus {
    private final Path root;

    public AgentFlowDataBus(String rootDir) throws IOException {
        this.root = Paths.get(rootDir == null || rootDir.isEmpty() ? System.getenv("AF_DATABUS_DIR") : rootDir);
        Files.createDirectories(root.resolve("kv"));
        Files.createDirectories(root.resolve("events"));
    }

    public void set(String key, String value) throws IOException {
        String encoded = Base64.getEncoder().encodeToString(value.getBytes(StandardCharsets.UTF_8));
        Path target = root.resolve("kv").resolve(key.replace("/", "%2F") + ".kv");
        Files.writeString(target, "key=" + key + "\ntype=string\nvalue_b64=" + encoded + "\n");
    }
}
