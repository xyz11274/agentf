/**
 * AgentFlow DataBus Java SDK 基础用法示例
 *
 * 演示通过 UDS 连接 DataBus，进行 put/get/contains/remove 操作。
 *
 * 运行前提：AgentFlow Worker 已启动并设置 AF_DATABUS_UDS_FILE 环境变量。
 * 也可以通过命令行参数指定 socket 路径：
 *   java BasicUsage /tmp/agentflow_session_xxx.sock
 *
 * 编译：
 *   javac -cp ../target/agentflow-databus-1.0.0.jar BasicUsage.java
 * 运行：
 *   java -cp .:../target/agentflow-databus-1.0.0.jar BasicUsage
 */

import com.agentflow.databus.DataBusClient;
import com.agentflow.databus.DataBusException;

import java.io.IOException;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class BasicUsage {

    static void demoBasicOperations(DataBusClient client) throws DataBusException, IOException {
        System.out.println("\n=== 基础读写操作 ===");

        // 写入字符串值
        client.user().put("greeting", "Hello, AgentFlow!");
        System.out.println("put greeting: Hello, AgentFlow!");

        // 读取值
        Object value = client.user().get("greeting");
        System.out.println("get greeting: " + value);

        // 写入复杂对象（序列化为 JSON）
        Map<String, Object> config = new HashMap<>();
        config.put("debug", true);
        config.put("level", 5);
        config.put("tags", List.of("agent", "databus"));
        client.user().put("config", config);
        System.out.println("put config: " + config);

        Object retrieved = client.user().get("config");
        System.out.println("get config: " + retrieved);

        // 检查键是否存在
        boolean exists = client.user().contains("greeting");
        System.out.println("contains greeting: " + exists);

        boolean notExists = client.user().contains("nonexistent_key");
        System.out.println("contains nonexistent_key: " + notExists);

        // 删除键
        client.user().remove("greeting");
        System.out.println("remove greeting");

        boolean afterRemove = client.user().contains("greeting");
        System.out.println("contains greeting after remove: " + afterRemove);
    }

    static void demoNamespaces(DataBusClient client) throws DataBusException, IOException {
        System.out.println("\n=== 命名空间访问 ===");

        // sys 命名空间：框架通信
        client.sys().put("response", "OK");
        System.out.println("sys.response = " + client.sys().get("response"));

        // ctx 命名空间：上下文存储
        client.ctx().put("history", List.of("msg1", "msg2"));
        System.out.println("ctx.history = " + client.ctx().get("history"));

        // user 命名空间：用户自定义数据
        client.user().put("result", Map.of("status", "completed"));
        System.out.println("user.result = " + client.user().get("result"));
    }

    static void demoDialogueIsolation(DataBusClient client) throws DataBusException, IOException {
        System.out.println("\n=== 对话隔离 ===");

        String dialogueA = "dialogue-001";
        String dialogueB = "dialogue-002";

        // 两个对话写入相同键名，数据互不干扰
        client.user(dialogueA).put("skill_result", "Result from dialogue A");
        client.user(dialogueB).put("skill_result", "Result from dialogue B");

        Object resultA = client.user(dialogueA).get("skill_result");
        Object resultB = client.user(dialogueB).get("skill_result");
        System.out.println("Dialogue A skill_result: " + resultA);
        System.out.println("Dialogue B skill_result: " + resultB);
    }

    public static void main(String[] args) {
        DataBusClient client;
        try {
            if (args.length > 0) {
                client = new DataBusClient(args[0]);
            } else {
                client = DataBusClient.fromEnv();
            }
        } catch (IOException | DataBusException e) {
            System.err.println("连接失败: " + e.getMessage());
            System.err.println("请确认 AF_DATABUS_UDS_FILE 环境变量已设置，或通过参数指定 socket 路径");
            System.exit(1);
            return;
        }

        System.out.println("连接到 DataBus: " + client.getSocketPath());
        System.out.println("Session ID: " + client.getSessionId());

        try (client) {
            demoBasicOperations(client);
            demoNamespaces(client);
            demoDialogueIsolation(client);
        } catch (DataBusException e) {
            System.err.println("DataBus 错误 " + e.getCode() + ": " + e.getMessage());
            System.exit(1);
        } catch (IOException e) {
            System.err.println("IO 错误: " + e.getMessage());
            System.exit(1);
        }

        System.out.println("\n示例运行完毕。");
    }
}
