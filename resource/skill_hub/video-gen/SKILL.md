---
name: video-gen
description: Generate a short video from text or animate a still image with Alibaba Cloud Wan models through a trusted structured tool. Produces an MP4 and reports business success or failure without exposing provider credentials.
version: 1.0.0
---

# Video Gen — text-to-video & image-to-video with Wan (通义万相)

Generate a short **MP4** video with Alibaba Cloud DashScope's **Wan / 通义万相**
video-synthesis models. Two modes:

- **Text-to-video** — describe a scene and get a clip (`--prompt` only).
- **Image-to-video** — set `image` to a workspace first frame and use
  `prompt` to describe the motion.

Use the structured `generate_video` tool. Never execute `make_video.py`
through `bash`, inspect the environment, or pass a provider credential.

## How to use

Text-to-video:

```json
{
  "prompt": "一只猫在阳光下的窗台上伸懒腰，电影感镜头",
  "output": "cat.mp4"
}
```

Image-to-video:

```json
{
  "image": "assets/hero.jpg",
  "prompt": "镜头缓缓推进，云在背景流动",
  "output": "hero.mp4"
}
```

- `prompt` describes content/motion and is required for text-to-video.
- `image` is an optional workspace image path; when present it becomes the first frame.
- `model`, `size`, `resolution`, `ratio`, `duration`, `audio_url`,
  `negative_prompt`, and `poll_timeout` are optional.
- `output` is required and must be a workspace media path such as `videos/look.mp4`.
- The result has business `status` plus a `files` array. Attach the returned
  file on success. On failure, surface the error; do not retry by asking for or
  searching for credentials.

## Models — pick per task (don't hard-bind one)

The tool is not tied to a single model: it selects text-to-video vs
image-to-video from the `image` field and chooses the request protocol from
the model id (legacy `size` for wanx2.1/wan2.2; new
`resolution`+`ratio`+`duration` for wan2.5/2.6/2.7+).
Choose the `model` field per task based on speed, quality, and mode. Rule of thumb:

- need it **fast / cheap** → a `*-turbo` (2.1) or `*-flash` (2.2/2.6) variant
- need **best quality** → a `*-plus` variant or a newer generation
- **lip-sync / 数字人** (image + audio) → `wan2.2-s2v` (different input shape — see docs)

| model id | mode | notes |
|---|---|---|
| `wanx2.1-t2v-turbo` | text-to-video | fast & cheap — **the script's default** (proven request shape) |
| `wanx2.1-t2v-plus` | text-to-video | higher quality, slower/pricier |
| `wan2.2-t2v-plus` | text-to-video | 2.2 gen, stronger motion/quality |
| `wanx2.1-i2v-turbo` | image-to-video | **default when `image` is given** |
| `wanx2.1-i2v-plus` | image-to-video | higher-quality i2v |
| `wan2.2-i2v-flash` / `wan2.2-i2v-plus` | image-to-video | 2.2 gen (flash = faster, plus = quality) |
| `wan2.6-i2v-flash` / `wan2.6-i2v` | image-to-video | **2.6 gen — latest i2v**, multi-shot narrative |
| `wan2.5-i2v-preview` | image-to-video | preview, can auto-generate audio |
| `wanx2.1-kf2v-plus` | first+last frame → video | interpolates between two keyframes |
| `wan2.7-t2v` | text-to-video | **latest t2v** — new protocol, supported (verified end-to-end) |
| `wan2.2-s2v` | speech-to-video (数字人) | image + audio → lip-sync; different input shape |

### Caveats when picking a model

- **Protocol auto-selected by model id**: the backend sends the legacy payload
  (`parameters.size`) for `wanx2.1*`/`wan2.2*`, and the new payload
  (`parameters.resolution`/`ratio`/`duration`) for `wan2.5*`/`wan2.6*`/`wan2.7*`
  and newer. Use `resolution`/`ratio`/`duration` with new-protocol models
  and `size` with legacy ones. `wan2.2-s2v` needs a different input shape and
  is not covered by this tool.
- **No live model list for video**: the DashScope `/models` endpoint only lists
  image/chat models (it's how you'd discover the latest `wan2.7-image`), but
  video-synthesis models are NOT enumerable there. So you can't auto-query the
  latest video model at runtime — rely on this table + the docs, and if a call
  rejects the model, pass a known-good id in `model`.
- `size`/resolution support differs per model (turbo/plus usually 480P/720P;
  newer generations support higher).

## Notes

- The trusted executor supplies the configured provider credential internally.
  Never ask for it, inspect it, or pass it in tool arguments. If configuration
  is missing, surface the returned business error.
- **This is asynchronous and slow** — generation typically takes 1–5 minutes.
  The script submits the task, polls every ~10 s, and downloads the result; just
  let it run. It prints progress to stderr.
- One call = one clip. For a multi-shot sequence, call the script per shot and
  stitch them afterwards.
- Match the prompt's language to the user's. Be concrete about subject, motion,
  camera, and style for better results.
- Pair with `voice-synth` (add narration) or `forge-image-gen` (make a first
  frame, then animate it).
