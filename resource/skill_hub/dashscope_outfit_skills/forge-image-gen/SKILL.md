---
name: forge-image-gen
description: Shared text-to-image backend that other forge skills call. Turns a prompt into image file(s) through a pluggable provider registry (DashScope z-image-turbo default + opt-in Alibaba 通义万相 Wan 2.x, plus OpenAI; Gemini/OpenRouter are one registry entry away). Reads the provider key from the environment; zero pip dependencies.
preload: references/providers.md
requires_secrets: DASHSCOPE_API_KEY
requires_network: dashscope.aliyuncs.com
version: 1.1.0
tags:
- agentfoundry
- 多模态
---

# Forge Image Gen

A **shared base skill** — it is not run on its own. It is the image engine that
content skills (`forge-xhs-images`, `forge-cover-image`, `forge-infographic`,
`forge-comic`, …) call. Mount it on the same agent alongside any skill that
needs to produce images.

## How other skills call it

Call the structured `generate_image` tool. Never run `generate.py` through
`bash` and never pass, print, probe, or request a provider credential.

```json
{
  "prompt": "a minimalist mountain logo, flat vector, indigo on cream",
  "output": "assets/logo.png",
  "size": "1024x1024"
}
```

- `prompt` and `output` are required.
- `provider` selects `dashscope`, `wan`, or `openai`; omit it for automatic selection.
- `model`, `size`, and `n` are optional; `n` is limited to 1–4.
- `refs` is an array of workspace image paths for image-to-image.
- The result has business `status` plus a `files` array. A failed image does
  not mean the Harness workflow failed.

## Provider credentials

Credentials are read only by the trusted tool in the Skill Executor process.
They are never copied into prompts, tool arguments, generic subprocesses, or
sandbox files. If no configured provider is available, report the returned
business error; do not inspect the environment or retry through `bash`.

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

- Generate one image per `generate_image` call for fine-grained control, or use
  `n` when the provider supports batches.
- Always pass an explicit workspace-relative `output` such as
  `assets/logo.png` or `image-cards/card.png`.
- Check the returned business `status`; surface failures honestly without
  attempting to recover credentials from the environment.
