---
name: video-gen
description: Generate a short video from a text prompt (text-to-video) or animate a still image (image-to-video) with Alibaba Cloud's Wan / 通义万相 video models (DashScope). Use whenever the user wants to create / generate / make a video, a clip, an animation, or bring a photo to life. Produces an MP4. Asynchronous — submits a task and polls until the video is ready. Zero pip dependencies; reads DASHSCOPE_API_KEY from the environment.
version: 1.0.0
tags:
- agentfoundry
- 多模态
---

# Video Gen — text-to-video & image-to-video with Wan (通义万相)

Generate a short **MP4** video with Alibaba Cloud DashScope's **Wan / 通义万相**
video-synthesis models. Two modes:

- **Text-to-video** — describe a scene and get a clip (`--prompt` only).
- **Image-to-video** — animate a still image; `--image` is the first frame and
  `--prompt` describes the motion.

`${SKILL_DIR}` is this skill's directory; the script is at `${SKILL_DIR}/scripts/make_video.py`.

## How to use

```
python3 ${SKILL_DIR}/scripts/make_video.py --prompt "一只猫在阳光下的窗台上伸懒腰，电影感镜头" --out cat.mp4
python3 ${SKILL_DIR}/scripts/make_video.py --image hero.jpg --prompt "镜头缓缓推进，云在背景流动" --out hero.mp4
```

- `--prompt` — what to generate / how it should move. Required for text-to-video.
- `--image` — a sandbox file path OR an `http(s)://` URL. If given, switches to
  **image-to-video** (the image is the opening frame). Local files are
  base64-encoded automatically.
- `--model` — Wan model id. Defaults: text-to-video `wanx2.1-t2v-turbo`,
  image-to-video `wanx2.1-i2v-turbo` (or set `DASHSCOPE_VIDEO_MODEL`). Higher
  quality: `wanx2.1-t2v-plus` / `wanx2.1-i2v-plus`.
- `--size` — text-to-video frame size for the **legacy** protocol
  (wanx2.1/wan2.2), default `1280*720`; also `960*960`, `720*1280`, …. Ignored
  for image-to-video (derived from the image) and for the new protocol.
- `--resolution` — frame resolution for the **new** protocol (wan2.5/2.6/2.7),
  default `720P`; also `1080P`.
- `--ratio` — aspect ratio for new-protocol text-to-video: `16:9` (default),
  `9:16`, `1:1`, `4:3`, `3:4`.
- `--duration` — clip length in seconds for the new protocol (default `5`).
- `--audio-url` — optional audio track URL (new protocol, wan2.5/2.6+).
- `--negative-prompt` — things to avoid (optional).
- `--out` — output MP4 path. Default `video.mp4` in the current directory.
- `--poll-timeout` — max seconds to wait for the task (default 600).

The script prints the path of the written MP4 to stdout. Read that path and
**attach the video as a deliverable** so the user can play / download it.

## Models — pick per task (don't hard-bind one)

The script is NOT tied to a single model: it auto-selects text-to-video vs
image-to-video from whether `--image` is given, picks the right request
**protocol** from the model id (legacy `size` for wanx2.1/wan2.2; new
`resolution`+`ratio`+`duration` for wan2.5/2.6/2.7+), and `--model` is just a
fallback default. **Choose the model per task** and pass it via `--model` (or
set `DASHSCOPE_VIDEO_MODEL`) based on what the user needs — speed vs quality,
plain clip vs lip-sync, etc. Rule of thumb:

- need it **fast / cheap** → a `*-turbo` (2.1) or `*-flash` (2.2/2.6) variant
- need **best quality** → a `*-plus` variant or a newer generation
- **lip-sync / 数字人** (image + audio) → `wan2.2-s2v` (different input shape — see docs)

| model id | mode | notes |
|---|---|---|
| `wanx2.1-t2v-turbo` | text-to-video | fast & cheap — **the script's default** (proven request shape) |
| `wanx2.1-t2v-plus` | text-to-video | higher quality, slower/pricier |
| `wan2.2-t2v-plus` | text-to-video | 2.2 gen, stronger motion/quality |
| `wanx2.1-i2v-turbo` | image-to-video | **default when `--image` is given** |
| `wanx2.1-i2v-plus` | image-to-video | higher-quality i2v |
| `wan2.2-i2v-flash` / `wan2.2-i2v-plus` | image-to-video | 2.2 gen (flash = faster, plus = quality) |
| `wan2.6-i2v-flash` / `wan2.6-i2v` | image-to-video | **2.6 gen — latest i2v**, multi-shot narrative |
| `wan2.5-i2v-preview` | image-to-video | preview, can auto-generate audio |
| `wanx2.1-kf2v-plus` | first+last frame → video | interpolates between two keyframes |
| `wan2.7-t2v` | text-to-video | **latest t2v** — new protocol, supported (verified end-to-end) |
| `wan2.2-s2v` | speech-to-video (数字人) | image + audio → lip-sync; different input shape |

### Caveats when picking a model

- **Protocol auto-selected by model id**: the script sends the legacy payload
  (`parameters.size`) for `wanx2.1*`/`wan2.2*`, and the new payload
  (`parameters.resolution`/`ratio`/`duration`) for `wan2.5*`/`wan2.6*`/`wan2.7*`
  and newer — so both generations Just Work. Use `--resolution`/`--ratio`/
  `--duration` with new-protocol models and `--size` with legacy ones (the
  irrelevant flag is ignored). Note: `wan2.2-s2v` (lip-sync) needs a different
  endpoint/input (audio) and is NOT covered by this script.
- **No live model list for video**: the DashScope `/models` endpoint only lists
  image/chat models (it's how you'd discover the latest `wan2.7-image`), but
  video-synthesis models are NOT enumerable there. So you can't auto-query the
  latest video model at runtime — rely on this table + the docs, and if a call
  404s on the model, pass a known-good id with `--model`.
- `--size`/resolution support differs per model (turbo/plus usually 480P/720P;
  newer generations support higher).

## Notes

- Requires `DASHSCOPE_API_KEY` in the environment (configure it via the
  Environment's secrets, like the other DashScope skills). If it's missing the
  script exits with a clear message — surface that to the user.
- **This is asynchronous and slow** — generation typically takes 1–5 minutes.
  The script submits the task, polls every ~10 s, and downloads the result; just
  let it run. It prints progress to stderr.
- One call = one clip. For a multi-shot sequence, call the script per shot and
  stitch them afterwards.
- Match the prompt's language to the user's. Be concrete about subject, motion,
  camera, and style for better results.
- Pair with `voice-synth` (add narration) or `forge-image-gen` (make a first
  frame, then animate it).
