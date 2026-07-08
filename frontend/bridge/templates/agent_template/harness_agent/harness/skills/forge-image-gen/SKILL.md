---
name: forge-image-gen
description: Shared text-to-image backend that other forge skills call. Turns a prompt into image file(s) through a pluggable provider registry (DashScope z-image-turbo default + opt-in Alibaba 通义万相 Wan 2.x, plus OpenAI; Gemini/OpenRouter are one registry entry away). Reads the provider key from the environment; zero pip dependencies.
preload: references/providers.md
requires_secrets: DASHSCOPE_API_KEY
requires_network: dashscope.aliyuncs.com
version: 1.1.0
---

# Forge Image Gen

A **shared base skill** — it is not run on its own. It is the image engine that
content skills (`forge-xhs-images`, `forge-cover-image`, `forge-infographic`,
`forge-comic`, …) call. Mount it on the same agent alongside any skill that
needs to produce images.

## How other skills call it

After materialization the script is at
`${SKILL_DIR}/scripts/generate.py`. Invoke it with bash:

```bash
python3 ${SKILL_DIR}/scripts/generate.py \
  --prompt "a minimalist mountain logo, flat vector, indigo on cream" \
  --output ./assets/logo.png \
  --size 1024x1024
```

- `--prompt`/`-p` and `--output`/`-o` are required.
- `--provider` forces a backend; omit it and the first provider with a key set wins.
- `--model`/`-m`, `--size`, `--n` are optional.
- `--ref <path>` (repeatable) supplies reference image(s) for image-to-image.
- `--list-providers` prints the registry; `--json` emits a machine-readable result.
- The output extension is optional (`.png` default); parent directories are created.
- For `--n > 1`, files get a `-1`, `-2`, … suffix.

## Providers & keys

The script reads each provider's API key from an environment variable
(`OPENAI_API_KEY`, …). **Sandbox prerequisite:** AgentFoundry's sandbox drops
`*_API_KEY` from the per-call extra-env path by default, so the key must reach
the sandbox either through the environment whitelist or — recommended for
on-prem — via Vaults injection (`Environment.config.secrets`, shipped v1.9).
`references/providers.md` has the exact operator setup — without it, no image
can be generated.

## Scope

- **DashScope (Alibaba) `z-image-turbo` (`dashscope` provider)** is the
  **default** backend — synchronous multimodal endpoint, key `DASHSCOPE_API_KEY`,
  first in `SELECTION_ORDER`, so an unspecified `--provider` uses z-image when
  the key is set. Fast and light. Verified live: a real 1024×1024 PNG.
- **Alibaba 通义万相 Wan 2.x (`wan` provider, default `wan2.7-image`)** —
  higher fidelity, **opt-in** on the **same** key: request it with
  `--provider wan` (e.g. when the user asks for Wan 2.7). Heavier variants:
  `-m wan2.7-image-pro` (composition + up to 4096²) / `-m wan2.6-image`. Wan
  calls force `watermark:false` and `prompt_extend:false` so the prompt is
  honoured verbatim (e.g. "no text").
- **OpenAI Images** is also implemented (`gpt-image-1` default; `-m dall-e-3` if
  your account isn't verified). Force it with `--provider openai`.
- The `PROVIDERS` registry in `generate.py` is the single extension point —
  adding Gemini/OpenRouter is one entry plus one `generate(...)` function.
- Image-to-image is supported via `--ref <path>` (repeatable): reference
  image(s) condition the model on real pixels — powers anchor-chain consistency
  (generate an anchor, then condition the rest on it). On **OpenAI** the refs go
  to the images/edits endpoint; on **DashScope/Wan** a ref switches the model to
  `qwen-image-edit-plus` and the anchor rides along as a base64 data-URI on the
  same endpoint (so a single `DASHSCOPE_API_KEY` covers it — no object storage).

## Notes for the calling skill

- Generate one image per `generate.py` call for fine-grained control, or use
  `--n` when the provider supports batches.
- Always pass an explicit `--output` under the working directory (e.g.
  `./assets/…` or `./image-cards/…`) so the artifact lands where your skill's
  `post_run`/delivery step expects it.
- Check the exit code: non-zero means generation failed (missing key, provider
  error) and stderr carries the reason.
