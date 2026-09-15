import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
public final class ClientProbe {
    public static void main(String[] args) throws Exception {
        var json = new ObjectMapper();
        var client = new Evolution(System.getenv("EVOLUTION_URL"), System.getenv("EVOLUTION_TEST_TOKEN"), 7000, 2097152, 0);
        var input = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        for (String line; (line = input.readLine()) != null;) {
            ObjectNode request = (ObjectNode)json.readTree(line);
            String operation = request.remove("operation").asText(), id = request.remove("request_id").asText();
            ObjectNode reply = json.createObjectNode();
            try { reply.put("ok", true).set("result", client.invoke(operation, request, id)); }
            catch (Evolution.Failure error) { reply.put("ok", false).put("error", error.getMessage()).put("status", error.status); }
            System.out.println(json.writeValueAsString(reply));
        }
    }
}
