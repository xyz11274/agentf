// Deterministic entry protocol fixture; not an Agent benchmark implementation.
import { Evolution, evaluate } from "../../clients/javascript/evolution.mjs";
const client = new Evolution({ url: "https://deployment.invalid", token: "deployment-placeholder-credential", retries: 0 });
await evaluate(async message => {
  if(message.history.length !== message.turn * 2 + 1) throw new Error("history_invalid");
  const run = await client.point({ phase: "before", kind: "run", ref: {run: message.key},
    conversation_id: message.conversation, input: message.text }, 5000);
  if(run.degraded) throw new Error("capture_failed");
  const result = await client.point({ phase: "after", kind: "run", ref: {run: message.key},
    output: { answer: "20", state: {value: 20} }, business_status: "succeeded" }, 5000);
  if(result.degraded) throw new Error("capture_failed");
});
