import java.io.*;
import java.util.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;

/**
 * Hot Reload Test JAR
 * Version 1.0
 *
 * This JAR provides test functions for hot reload functionality.
 * All functions are static methods that can be called via command-line.
 */
public class HotReloadTest {

    private static final String VERSION = "1.0";

    public static void main(String[] args) {
        if (args.length == 0) {
            printHelp();
            System.exit(1);
        }

        String command = args[0];

        switch (command) {
            case "execute":
                execute(Arrays.copyOfRange(args, 1, args.length));
                break;
            case "add":
                add(Arrays.copyOfRange(args, 1, args.length));
                break;
            case "multiply":
                multiply(Arrays.copyOfRange(args, 1, args.length));
                break;
            case "echo":
                echo(Arrays.copyOfRange(args, 1, args.length));
                break;
            case "version":
                printVersion();
                break;
            case "info":
                printInfo();
                break;
            case "sleep":
                sleep(Arrays.copyOfRange(args, 1, args.length));
                break;
            case "file-read":
                fileRead(Arrays.copyOfRange(args, 1, args.length));
                break;
            case "file-write":
                fileWrite(Arrays.copyOfRange(args, 1, args.length));
                break;
            default:
                System.err.println("[JAR] Unknown command: " + command);
                printHelp();
                System.exit(1);
        }
    }

    /**
     * Execute test - processes inputs and outputs
     */
    private static void execute(String[] args) {
        System.out.println("[JAR] execute called, Version: " + VERSION);
        System.out.println("[JAR] Java version: " + System.getProperty("java.version"));

        Map<String, String> inputs = parseInputs(args);

        // Process inputs
        System.out.println("[JAR] Inputs: " + inputs.size());
        for (Map.Entry<String, String> entry : inputs.entrySet()) {
            System.out.println("[JAR]   " + entry.getKey() + " = " + entry.getValue());
        }

        // Generate outputs
        Map<String, String> outputs = new HashMap<>();
        outputs.put("result", "success");
        outputs.put("status", "ok");
        outputs.put("version", VERSION);
        outputs.put("timestamp", String.valueOf(System.currentTimeMillis() / 1000));
        outputs.put("processed", String.valueOf(inputs.size()));

        // Print outputs
        System.out.println("[JAR] Outputs:");
        for (Map.Entry<String, String> entry : outputs.entrySet()) {
            System.out.println("[JAR]   " + entry.getKey() + " = " + entry.getValue());
        }

        // Simulate work
        try {
            Thread.sleep(100);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }

        System.out.println("[JAR] Execution complete");
    }

    /**
     * Add test - adds two numbers
     */
    private static void add(String[] args) {
        System.out.println("[JAR] add called");

        int a = 0, b = 0;

        if (args.length >= 2) {
            try {
                a = Integer.parseInt(args[0]);
                b = Integer.parseInt(args[1]);
            } catch (NumberFormatException e) {
                System.err.println("[JAR] Invalid number format");
                return;
            }
        }

        int result = a + b;

        System.out.println("[JAR] " + a + " + " + b + " = " + result);

        Map<String, String> outputs = new HashMap<>();
        outputs.put("result", String.valueOf(result));
        outputs.put("a", String.valueOf(a));
        outputs.put("b", String.valueOf(b));
        outputs.put("operation", "add");

        printOutputs(outputs);
    }

    /**
     * Multiply test - multiplies two numbers
     */
    private static void multiply(String[] args) {
        System.out.println("[JAR] multiply called");

        int a = 1, b = 1;

        if (args.length >= 2) {
            try {
                a = Integer.parseInt(args[0]);
                b = Integer.parseInt(args[1]);
            } catch (NumberFormatException e) {
                System.err.println("[JAR] Invalid number format");
                return;
            }
        }

        int result = a * b;

        System.out.println("[JAR] " + a + " * " + b + " = " + result);

        Map<String, String> outputs = new HashMap<>();
        outputs.put("result", String.valueOf(result));
        outputs.put("a", String.valueOf(a));
        outputs.put("b", String.valueOf(b));
        outputs.put("operation", "multiply");

        printOutputs(outputs);
    }

    /**
     * Echo test - prints arguments
     */
    private static void echo(String[] args) {
        System.out.println("[JAR] echo called");
        System.out.println("[JAR] Arguments: " + String.join(", ", args));

        Map<String, String> outputs = new HashMap<>();
        outputs.put("count", String.valueOf(args.length));
        outputs.put("args", String.join("|", args));

        printOutputs(outputs);
    }

    /**
     * Version test - prints version information
     */
    private static void printVersion() {
        System.out.println("[JAR] printVersion called");

        Map<String, String> outputs = new HashMap<>();
        outputs.put("name", "Hot Reload Test JAR");
        outputs.put("version", VERSION);
        outputs.put("java_version", System.getProperty("java.version"));
        outputs.put("java_vendor", System.getProperty("java.vendor"));
        outputs.put("os_name", System.getProperty("os.name"));
        outputs.put("os_arch", System.getProperty("os.arch"));
        outputs.put("os_version", System.getProperty("os.version"));

        printOutputs(outputs);
    }

    /**
     * Info test - prints library information
     */
    private static void printInfo() {
        System.out.println("[JAR] printInfo called");

        Map<String, String> outputs = new HashMap<>();
        outputs.put("library", "Hot Reload Test JAR");
        outputs.put("version", VERSION);
        outputs.put("author", "AgentFlow");
        outputs.put("description", "Test JAR for hot reload functionality");
        outputs.put("build_date", new Date().toString());
        outputs.put("supported_commands", "execute,add,multiply,echo,version,info,sleep,file-read,file-write");

        printOutputs(outputs);
    }

    /**
     * Sleep test - sleeps for specified duration
     */
    private static void sleep(String[] args) {
        long durationMs = 1000; // default 1 second

        if (args.length > 0) {
            try {
                durationMs = Long.parseLong(args[0]);
            } catch (NumberFormatException e) {
                System.err.println("[JAR] Invalid duration format, using default");
            }
        }

        System.out.println("[JAR] sleep called, duration: " + durationMs + "ms");

        try {
            Thread.sleep(durationMs);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }

        Map<String, String> outputs = new HashMap<>();
        outputs.put("result", "success");
        outputs.put("slept_ms", String.valueOf(durationMs));

        printOutputs(outputs);
    }

    /**
     * File read test - reads from a file
     */
    private static void fileRead(String[] args) {
        if (args.length == 0) {
            System.err.println("[JAR] No file specified");
            return;
        }

        String filePath = args[0];
        System.out.println("[JAR] fileRead called, file: " + filePath);

        try {
            String content = new String(Files.readAllBytes(Paths.get(filePath)), StandardCharsets.UTF_8);

            Map<String, String> outputs = new HashMap<>();
            outputs.put("result", "success");
            outputs.put("file_path", filePath);
            outputs.put("file_size", String.valueOf(content.length()));
            outputs.put("content_preview", content.length() > 100 ? content.substring(0, 100) + "..." : content);

            printOutputs(outputs);
        } catch (IOException e) {
            System.err.println("[JAR] File read error: " + e.getMessage());
            Map<String, String> outputs = new HashMap<>();
            outputs.put("result", "error");
            outputs.put("error", e.getMessage());
            printOutputs(outputs);
        }
    }

    /**
     * File write test - writes to a file
     */
    private static void fileWrite(String[] args) {
        if (args.length == 0) {
            System.err.println("[JAR] No content specified");
            return;
        }

        String content = String.join(" ", args);
        String filePath = "hot_reload_test_output.txt";

        System.out.println("[JAR] fileWrite called, file: " + filePath);

        try {
            Files.write(Paths.get(filePath), content.getBytes(StandardCharsets.UTF_8));

            Map<String, String> outputs = new HashMap<>();
            outputs.put("result", "success");
            outputs.put("file_path", filePath);
            outputs.put("content_length", String.valueOf(content.length()));

            printOutputs(outputs);
        } catch (IOException e) {
            System.err.println("[JAR] File write error: " + e.getMessage());
            Map<String, String> outputs = new HashMap<>();
            outputs.put("result", "error");
            outputs.put("error", e.getMessage());
            printOutputs(outputs);
        }
    }

    /**
     * Parse inputs from command line arguments
     */
    private static Map<String, String> parseInputs(String[] args) {
        Map<String, String> inputs = new HashMap<>();

        for (String arg : args) {
            if (arg.contains("=")) {
                String[] parts = arg.split("=", 2);
                if (parts.length == 2) {
                    inputs.put(parts[0], parts[1]);
                }
            } else {
                inputs.put("arg_" + inputs.size(), arg);
            }
        }

        return inputs;
    }

    /**
     * Print outputs in standardized format
     */
    private static void printOutputs(Map<String, String> outputs) {
        System.out.println("[JAR] Outputs:");
        for (Map.Entry<String, String> entry : outputs.entrySet()) {
            System.out.println("[JAR]   " + entry.getKey() + "=" + entry.getValue());
        }
    }

    /**
     * Print help information
     */
    private static void printHelp() {
        System.out.println("Hot Reload Test JAR v" + VERSION);
        System.out.println();
        System.out.println("Usage: java -jar HotReloadTest.jar <command> [args...]");
        System.out.println();
        System.out.println("Commands:");
        System.out.println("  execute [key=value...]  - Execute test with inputs");
        System.out.println("  add <a> <b>           - Add two numbers");
        System.out.println("  multiply <a> <b>       - Multiply two numbers");
        System.out.println("  echo <args...>          - Echo arguments");
        System.out.println("  version                  - Print version info");
        System.out.println("  info                    - Print library info");
        System.out.println("  sleep [ms]              - Sleep for specified ms");
        System.out.println("  file-read <path>         - Read file content");
        System.out.println("  file-write <content...> - Write content to file");
    }
}
