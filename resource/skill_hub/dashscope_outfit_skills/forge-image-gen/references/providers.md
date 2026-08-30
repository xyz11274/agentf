# Providers & trusted credential isolation

## Provider table

| Provider | Env var(s) | Default model | Status |
|----------|-----------|---------------|--------|
| `dashscope` | `DASHSCOPE_API_KEY` (+ optional `DASHSCOPE_BASE_URL`) | `z-image-turbo` | ✅ implemented — **auto-selected default** |
| `wan` | `DASHSCOPE_API_KEY` (+ optional `DASHSCOPE_BASE_URL`) | `wan2.7-image` | ✅ implemented — opt-in (`--provider wan`) |
| `openai` | `OPENAI_API_KEY` (+ optional `OPENAI_BASE_URL`) | `gpt-image-1` | ✅ implemented |
| `google` | `GEMINI_API_KEY` or `GOOGLE_API_KEY` | imagen / gemini image | ⏳ registry-ready |
| `openrouter` | `OPENROUTER_API_KEY` | — | ⏳ registry-ready |

`SELECTION_ORDER = ["dashscope", "wan", "openai"]` — with `DASHSCOPE_API_KEY`
set, an unspecified `--provider` resolves to **z-image-turbo** (the fast/light
default). **Alibaba 通义万相 Wan 2.7** (`wan2.7-image`) is opt-in: request it
with `--provider wan` (e.g. when the user asks for it). `wan` and `dashscope`
share the same key and the same synchronous multimodal endpoint
(`/api/v1/services/aigc/multimodal-generation/generation`); they differ only in
the model id and a couple of Wan parameters (`watermark:false`,
`prompt_extend:false`). Heavier Wan variants: `-m wan2.7-image-pro`
(composition + up to 4096²) / `-m wan2.6-image`.

Add a provider by registering one entry in `PROVIDERS` (env var names + default
model + a `generate(prompt, *, model, size, n)` function returning a list of
image byte-strings) and appending its id to `SELECTION_ORDER`.

## Credential boundary

Provider credentials belong to the trusted Harness process. They are never
copied into a local or E2B sandbox, exposed to Bash, embedded in tool arguments,
or written to the workspace.

The supported execution path is:

1. The operator configures the credential for the Harness daemon through its
   normal secret store or host environment.
2. The structured `generate_image` tool selects the provider and reads the
   credential internally. The agent supplies only generation parameters such
   as `prompt`, `output`, `model`, and optional workspace reference paths.
3. The trusted wrapper performs a direct HTTPS request with certificate
   verification enabled and redirects disabled. Credential-bearing endpoint
   URLs are rejected.
4. Only validated media bytes cross into or out of the sandbox. Before an
   artifact is published, its path and content are checked for registered
   credential material.
5. The tool returns sanitized status and artifact metadata. Provider errors are
   redacted before they reach the agent, logs, DataBus, or trajectory hooks.

Never add provider variables to a sandbox environment whitelist, inject a vault
secret into a sandbox, probe credentials with shell commands, or invoke
`scripts/generate.py` through Bash. Those paths defeat the isolation boundary
and are intentionally unsupported.

## Adding a provider safely

A provider implementation may read its credential only inside the trusted
backend called by `generate_image`. It must:

- register every credential source before constructing requests;
- require HTTPS for non-loopback endpoints and keep TLS verification enabled;
- avoid redirects and ambient proxy forwarding for authenticated requests;
- never put a credential in a URL, prompt, output path, exception, or artifact;
- return image byte strings to the trusted wrapper instead of writing directly
  into an arbitrary workspace path.

Test provider integrations through the structured tool with a mocked transport.
A real smoke test may use a credential configured on the trusted daemon, but
the test must never print, persist, hash, or pass that value to the sandbox.
