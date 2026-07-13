---
name: forge-image-gen
description: Minimax text-to-image engine. Turns a prompt into image file(s) through MiniMax Image-01. Reads API_KEY from the environment; zero pip dependencies. Used by outfit-stylist and other content skills to generate outfit / fashion images.
preload: references/providers.md
requires_secrets: API_KEY
requires_network: api.minimaxi.com
version: 1.1.0
tags:
- agentfoundry-local-test
- 多模态
---

# Forge Image Gen (MiniMax)

A **shared base skill** — it is not run on its own. It is the image engine that
content skills (`outfit-stylist`, …) call. Mount it on the same agent alongside
any skill that needs to produce images.

## How other skills call it

After materialization the script is at
`${SKILL_DIR}/scripts/generate.py`. Invoke it with bash:

```bash
python3 ${SKILL_DIR}/scripts/generate.py \
  --prompt "a minimalist mountain logo, flat vector, indigo on cream" \
  --output ./assets/logo.png \
  --aspect-ratio 1:1
```

- `--prompt`/`-p` and `--output`/`-o` are required.
- `--aspect-ratio` / `--ar`: 16:9 (default), 4:3, 3:2, 2:3, 3:4, 9:16, 21:9
- `--model`/`-m`: model name, default `image-01`
- `--n`: number of images (1–9, default 1)
- `--ref` / `--subject-reference`: reference image for subject-consistent generation.
  Accepts an `http(s)://` URL **or a local file path** (auto base64-encoded).
- `--timeout`: API timeout in seconds (default 60).
- The output extension is optional (`.png` default); parent directories are created.
- For `--n > 1`, files get a `-1`, `-2`, … suffix.

## Provider & key

Reads `API_KEY` (or `MINIMAX_API_KEY`) from the environment. Endpoint:
`https://api.minimaxi.com/v1/image_generation`.

## Scope

- **MiniMax Image-01** is the sole backend — fast, high-quality text-to-image
  and subject-reference image generation. No other providers are needed.
- Image-to-image with subject reference is supported via `--ref`.
- Output format: base64 JPEG images, decoded and written to disk.
