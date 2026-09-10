import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { once } from "node:events";
import fs from "node:fs";
import http from "node:http";
import net from "node:net";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Evolution } from "../clients/typescript/evolution.ts";
import { conversation } from "../examples/callback-agent/typescript.ts";

const source = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const build = path.resolve(process.argv[2] ?? "build/trajectory-evolution");
const root = fs.mkdtempSync(path.join(build, "integration-http."));
const a = "fixture-principal-alpha-00000001", b = "fixture-principal-bravo-00000002";
const reviewKey = "fixture-review-credential-00000003";
const env = { ...process.env, INTEGRATION_TEST_A: a, INTEGRATION_TEST_B: b, INTEGRATION_REVIEW_KEY: reviewKey };
let modelCalls = 0; let neutralContentSeen = false; const modelErrors: string[] = [];
const model = http.createServer(async (request, response) => {
  try {
    let body = ""; for await(const chunk of request) body += chunk;
    assert.equal(request.headers.authorization, "Bearer " + reviewKey);
    assert(!body.includes(a) && !body.includes(b) && !body.includes(reviewKey));
    const input = JSON.parse(body); ++modelCalls;
    const text = input.messages.find((m: any) => m.role === "user").content;
    const evidence = JSON.parse(text.split("输入证据：\n")[1].split("\n只返回一个 JSON 对象")[0]);
    if(JSON.stringify(evidence).includes("domain-extension-visible")) neutralContentSeen = true;
    const event = evidence.content_events.find((e: any) => e.event_id)?.event_id
      ?? evidence.episodes[0].evidence.source_event_ids[0];
    const ids = [event];
    const advice = { target_scope: evidence.allowed_experience_scopes[0], trigger_pattern: "视频生成缺少素材",
      proposed_change: "先检查当前可见的素材条件；缺失时保留其他独立任务，不重复执行不可恢复的生成请求。", evidence_event_ids: ids };
    const result = {
      task_outcome: { status: "partial", confidence: 0.8, reason: "独立分支完成，素材缺失的任务尚未完成。", evidence_event_ids: ids },
      episode_assessments: evidence.selected_episode_ids.map((episode_id: string) => ({ episode_id, verdict: "useful", confidence: 0.8,
        expected_state_change: "推进独立任务", observed_state_change: "已记录当前步骤结果", concise_reason: "保留真实执行结果供后续判断。", evidence_event_ids: ids })),
      reflection: { summary: "对前置条件不足的分支需要避免无依据重复。", root_causes: ["存在缺失素材的分支。"],
        skill_orchestration_changes: [], experience_injections: evidence.allowed_experience_scopes.length ? [advice] : [],
        stop_condition_changes: [], wasted_episode_ids: [], evidence_event_ids: ids },
    };
    response.writeHead(200, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ id: "fixture", choices: [{ finish_reason: "stop", message: { role: "assistant", content: JSON.stringify(result) } }], usage: { prompt_tokens: 100, completion_tokens: 50, total_tokens: 150 } }));
  } catch { modelErrors.push("model_fixture_contract_failed"); response.writeHead(500); response.end("{}"); }
});
model.listen(0, "127.0.0.1"); await once(model, "listening");
const fixture = spawnSync(path.join(build, "test_integration_service"), ["--fixture", root], { env, encoding: "utf8" });
assert.equal(fixture.status, 0, fixture.stderr);
const configPath = path.join(root, "service.json");
const config = JSON.parse(fs.readFileSync(configPath, "utf8"));
const runtime = JSON.parse(fs.readFileSync(config.runtime_config, "utf8"));
fs.cpSync(path.join(source, "config/prompts"), path.join(root, "prompts"), { recursive: true });
runtime.evaluation.model_selection = "all";
runtime.evaluation.strict_model = true;
runtime.evolution.enable_memory_semantic_matching = false;
runtime.model.endpoint = `http://127.0.0.1:${(model.address() as net.AddressInfo).port}/v1/chat/completions`;
runtime.model.model_id = "fixture-review"; runtime.model.api_key_env = "INTEGRATION_REVIEW_KEY";
runtime.model.thinking_mode = "disabled"; runtime.model.timeout_ms = 5000;
config.runtime_config = path.join(root, "evaluator.json"); config.http_timeout_ms = 800;
fs.writeFileSync(config.runtime_config, JSON.stringify(runtime)); fs.writeFileSync(configPath, JSON.stringify(config));

let server: ReturnType<typeof spawn>;
let stderr = "";
async function start(): Promise<string> {
  server = spawn(path.join(build, "trajectory_evolution"), ["serve", "--config", configPath], { env, stdio: ["ignore", "pipe", "pipe"] });
  server.stderr!.on("data", bytes => { stderr += bytes; });
  return new Promise((resolve, reject) => {
    let text = ""; const timer = setTimeout(() => reject(new Error("service_start_timeout")), 10000);
    server.on("exit", code => { clearTimeout(timer); reject(new Error("service_exit_" + code + ": " + stderr)); });
    server.stdout!.on("data", bytes => {
      text += bytes; const match = text.match(/listening=127\.0\.0\.1:(\d+)/);
      if(match) { clearTimeout(timer); resolve("http://127.0.0.1:" + match[1]); }
    });
  });
}
async function stop(): Promise<void> {
  if(!server || server.exitCode !== null) return;
  const exited = once(server, "exit"); server.kill("SIGTERM");
  const timer = setTimeout(() => server.kill("SIGKILL"), 8000);
  await exited; clearTimeout(timer);
}
function files(folder: string): string[] {
  return fs.readdirSync(folder, { withFileTypes: true }).flatMap(entry => entry.isDirectory() ? files(path.join(folder, entry.name)) : [path.join(folder, entry.name)]);
}
async function raw(url: string, payload: string): Promise<number> {
  return new Promise((resolve, reject) => {
    const socket = net.connect(Number(new URL(url).port), "127.0.0.1", () => socket.write(payload)); let text = "";
    socket.setTimeout(2500, () => socket.destroy(new Error("raw_request_timeout")));
    socket.on("data", bytes => { text += bytes; }); socket.on("error", reject);
    socket.on("end", () => resolve(Number(text.match(/^HTTP\/1\.1 (\d+)/)?.[1])));
  });
}
async function evaluated(run: any): Promise<void> {
  for(let i = 0; i < 150; ++i) {
    const status = await run.status();
    assert.notEqual(status.evaluation_status, "failed", status.evaluation_error);
    if(status.evaluation_status === "completed") return;
    await new Promise(resolve => setTimeout(resolve, 50));
  }
  throw new Error("evaluation_did_not_finish");
}

try {
  let url = await start(); let alpha = new Evolution({ url, token: a }); const beta = new Evolution({ url, token: b });
  // Two users, three conversations, two inputs each, independent active tasks.
  const scenarios = [[alpha, "ts-conv-a"], [alpha, "ts-conv-b"], [beta, "ts-conv-c"]] as const;
  const completed = [];
  for(let input = 0; input < 2; ++input) {
    const runs = await Promise.all(scenarios.map(([client, conv]) => conversation(client, conv)));
    completed.push(...runs); for(const run of runs) await evaluated(run);
  }
  assert(modelCalls >= 6, "model evaluation was silently disabled"); assert.deepEqual(modelErrors, []);
  await assert.rejects(beta.resume(completed[0].id).status(), /task_not_found/);
  const status = await completed[0].status(); assert(status.receipts.applied >= 4); assert(status.capture_complete);
  const memory = JSON.parse(fs.readFileSync(path.join(root, "data/by-user/user-a/memory/evolution-memory.json"), "utf8"));
  assert(memory.entries.some((e: any) => e.candidate.proposed_change.includes("先检查当前可见")), "model experience not admitted");
  // Direct L0/L1/L2 are also usable, not only the L3 convenience example.
  for(const level of ["L0", "L1", "L2"] as const) {
    const run = await alpha.start({ conversationId: "level-" + level, input: "视频生成缺少素材", level, capabilities: ["model", "actions", "loop"] });
    const step = await run.step({ progress: 1 }); await step.model({ system: "原指令" }, async req => ({ request_seen: req.system.length }));
    const [action] = await step.actions([{ id: "v", operation: "video", arguments: {} }]);
    assert.equal(action.handle.decision, level === "L2" ? "reject" : "allow");
    await action.execute(async () => ({})); await step.end(); await run.finish({ done: true }); await evaluated(run);
  }
  // The new profile reaches the same actual HTTP evaluator with custom content and dynamic actors.
  for(let i = 0; i < 2; ++i) {
    const run = await alpha.invoke("run.begin", { input: "视频生成缺少素材", conversation_id: "neutral-evaluation",
      level: "L1", capabilities: ["model"], extensions: { domain: "domain-extension-visible" } });
    const common = { run_id: run.run_id };
    const prepared = await alpha.invoke("activity.begin", { ...common, kind: "model", name: "native-model",
      input: { native: { instructions: "原有指令" } }, binding: { pointer: "/native/instructions", mode: "text" },
      actor_id: "ephemeral-worker-" + i, extensions: { rubric: "domain-extension-visible" } });
    assert(prepared.input.native.instructions.includes("先检查当前可见"), "neutral run did not use existing memory");
    await alpha.invoke("activity.end", { ...common, activity_id: prepared.activity_id, executed: true,
      actual_input: prepared.input, status: "completed", output: "独立业务完成" });
    await alpha.invoke("run.end", { ...common, status: "completed", business_status: "partial", output: "汇总真实内容" });
    await evaluated({ status: () => alpha.invoke("run.status", common) });
    assert((await alpha.invoke("run.status", common)).receipts.applied > 0);
  }
  assert(neutralContentSeen, "model evaluator did not receive neutral extensions/content");
  assert.deepEqual(modelErrors, []);
  const privacy = await alpha.start({ conversationId: "privacy", input: "只保留已授权业务内容", level: "L3", capabilities: ["model", "actions", "loop"] });
  const ps = await privacy.step({ ready: true });
  await ps.model({ system: "安全输入" }, async () => ({ api_key: "never-persist-this-fixture-credential" }));
  const [sibling] = await ps.actions([{ id: "safe", operation: "lookup", arguments: {} }]);
  assert.equal((await sibling.execute(async () => ({ done: true }))).status, "completed");
  await privacy.finish({ answer: "business result unaffected" });
  assert.equal((await privacy.status()).evaluation_status, "quarantined");
  const prefix = "POST /v1/invoke HTTP/1.1\r\nHost: localhost\r\n";
  assert.equal(await raw(url, prefix + "Content-Length: 2\r\nContent-Type: application/json\r\n\r\n{}"), 401);
  assert.equal(await raw(url, prefix + "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}"), 400);
  assert.equal(await raw(url, prefix + "Content-Length: 9999999\r\n\r\n"), 413);
  assert.equal(await raw(url, prefix + "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n"), 400);
  assert.equal(await raw(url, prefix + "Content-Length: 100\r\nContent-Type: application/json\r\nAuthorization: Bearer " + a + "\r\n\r\n{"), 408);
  // Native Rust binary, not a Python translation or a mocked HTTP client.
  const rustPath = process.env.EVOLUTION_RUST_EXAMPLE;
  if(rustPath) {
    const child = spawn(rustPath, [], { env: { ...env, EVOLUTION_URL: url, EVOLUTION_USER_A_TOKEN: a }, stdio: ["ignore", "pipe", "pipe"] });
    let output = "", error = ""; child.stdout.on("data", b => { output += b; }); child.stderr.on("data", b => { error += b; });
    const [code] = await once(child, "exit"); assert.equal(code, 0, error);
    const result = JSON.parse(output.trim()); await evaluated(alpha.resume(result.run_id));
  }
  // Recovery keeps an unfinished real call open until its callback or abort arrives.
  const pending = await alpha.start({ conversationId: "restart", input: "重启恢复", level: "L0", capabilities: [] });
  const openStep = await pending.step({ pending: true }); const call = await openStep.prepareModel({ system: "请求已交给宿主" });
  await stop(); url = await start(); alpha = new Evolution({ url, token: a });
  const recovered = alpha.resume(pending.id).resumeStep(openStep.handle);
  assert.equal((await recovered.run.status()).closed, false);
  await recovered.observeModel(call, call.request, { restored: true }); await recovered.end(); await recovered.run.finish({ done: true }); await evaluated(recovered.run);
  await stop();
  let traceCount = 0;
  for(const file of files(path.join(root, "data"))) {
    const bytes = fs.readFileSync(file);
    for(const value of [a, b, reviewKey, "never-persist-this-fixture-credential"]) assert(!bytes.includes(value), "credential persisted");
    if(file.endsWith(".trace.json") && !file.includes("/quarantine/")) {
      const check = spawnSync(path.join(build, "trajectory_evolution"), ["doctor", file, "--level", "L0"], { env, encoding: "utf8" });
      assert.equal(check.status, 0, check.stdout + check.stderr); ++traceCount;
    }
  }
  assert(!stderr.includes(a) && !stderr.includes(reviewKey));
  const result = { test_kind: "native-http-contract", users: 2, conversations: 3, inputs_per_conversation: 2,
    valid_traces: traceCount, model_calls: modelCalls, privacy_quarantined: true, rust_native_tested: Boolean(rustPath),
    artifact_directory: root };
  fs.writeFileSync(path.join(root, "test-summary.json"), JSON.stringify(result, null, 2)); console.log(JSON.stringify(result));
} finally { await stop(); model.closeAllConnections(); await new Promise<void>(resolve => model.close(() => resolve())); }
