# Providers & sandbox key injection

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

## How the API key reaches the sandbox

`generate.py` simply reads `os.environ["OPENAI_API_KEY"]`. The non-obvious part
is getting that variable *into* the sandbox, because `LocalSandbox.buildSafeEnv`
intentionally drops anything matching `*_API_KEY` on the per-call extra-env path
(defense-in-depth against accidental leaks).

Two supported routes. **The script is identical either way** — only the
injection mechanism differs.

### Route A — environment whitelist (works today, zero core changes)

`buildSafeEnv` copies `envWhitelist` entries straight from the host process env
*without* the `*_API_KEY` filter (that filter only guards the extra-env path).
So the operator adds the provider keys to the sandbox's `envWhitelist` when
constructing the factory, and exports them in the host environment:

```ts
new LocalSandboxFactory({
  workdirBase: '…',
  envWhitelist: [...DEFAULT_ENV_WHITELIST, 'OPENAI_API_KEY', 'OPENAI_BASE_URL'],
});
```

Simple, but the key lives in the host process env and applies to every session.

### Route B — Vaults injection (recommended for on-prem; shipped in v1.9)

Manage the provider key in the Vaults UI like any other secret and declare it on
the **Environment** the session runs in. AgentFoundry resolves it per
session/workspace at sandbox creation and injects it through a *trusted* channel
that bypasses the `*_API_KEY` guard **without weakening it** for the per-call
`opts.env` path:

```ts
// 1. The key lives in a vault (static_bearer), managed/rotated in the Vaults UI.
// 2. The Environment maps an env-var name → vault ref:
const env = await client.environments.create({
  name: 'image-gen',
  config: {
    type: 'cloud',
    secrets: { OPENAI_API_KEY: '${vault:image-providers/openai}' },
  },
});
// 3. Any session bound to `env` can read OPENAI_API_KEY from sandbox bash —
//    generate.py works unchanged. No host-env export, no envWhitelist edit.
```

The secret never lands in the host process env, the Environment stores only the
`${vault:...}` ref (not the resolved key), and scope is per-workspace. See
[`docs/user/guides/vaults.md` §4.4](../../../../docs/user/guides/vaults.md) for the full contract.

**Recommendation:** Route B is the productized path for the on-prem license
form. Route A still works for a quick single-key local spike.

## Quick local check

With a key exported in your shell, the script can be exercised directly:

```bash
export OPENAI_API_KEY=sk-…
python3 scripts/generate.py --list-providers
python3 scripts/generate.py -p "a teal origami crane on white" -o /tmp/crane.png
```

Without a key, `--list-providers` still works and a real generation exits
non-zero with a clear `OPENAI_API_KEY not set` message.
