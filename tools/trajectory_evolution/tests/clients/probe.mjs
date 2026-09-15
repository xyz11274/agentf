import readline from "node:readline";
import { Evolution, EvolutionError } from "../../clients/javascript/evolution.mjs";
const client = new Evolution({ url: process.env.EVOLUTION_URL, token: process.env.EVOLUTION_TEST_TOKEN, retries: 0 });
for await (const line of readline.createInterface({ input: process.stdin })) {
  const { operation, request_id, ...fields } = JSON.parse(line);
  try { console.log(JSON.stringify({ ok: true, result: await client.invoke(operation, fields, request_id) })); }
  catch (error) { if (!(error instanceof EvolutionError)) throw error; console.log(JSON.stringify({ ok: false, error: error.message, status: error.status })); }
}
