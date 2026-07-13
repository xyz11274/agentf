---
name: video-gen
description: Generate a short video from a text prompt (text-to-video) or animate a still image (image-to-video) with MiniMax Hailuo models. Use whenever the user wants to create / generate / make a video, a clip, an animation, or bring a photo to life. Produces an MP4. Asynchronous — submits a task and polls until the video is ready. Zero pip dependencies; reads API_KEY from the environment.
requires_secrets: API_KEY
requires_network: api.minimaxi.com
version: 1.0.0
---

# Video Gen — text-to-video & image-to-video with MiniMax Hailuo

Generate a short **MP4** video with MiniMax's **Hailuo** video-synthesis models.
Two modes:

- **Text-to-video** — describe a scene and get a clip (`--prompt` only).
- **Image-to-video** — animate a still image; `--image` is the first frame and
  `--prompt` describes the motion.

`${SKILL_DIR}` is this skill's directory; the script is at `${SKILL_DIR}/scripts/make_video.py`.

## How to use

```bash
# Text-to-video
python3 ${SKILL_DIR}/scripts/make_video.py \
  --prompt "a model walking on a runway in a elegant evening gown" \
  --out fashion.mp4

# Image-to-video (animate a static outfit photo)
python3 ${SKILL_DIR}/scripts/make_video.py \
  --image outfit.jpg \
  --prompt "model turns around slowly, fabric flowing" \
  --out outfit_turn.mp4
```

- `--prompt` — what to generate / how it should move. Required.
- `--image` — a local file path or an `http(s)://` URL. If given, switches to
  image-to-video (the image is the opening frame).
- `--model` — Hailuo model id. Default: `MiniMax-Hailuo-2.3` (or set
  `MINIMAX_VIDEO_MODEL`). Also accepts `MiniMax-Hailuo-02`, `MiniMax-Hailuo-2.3-Fast`.
- `--duration` — clip length in seconds (default 6, max 10 for Hailuo-02).
- `--resolution` — `1080P` (default) or `720P`.
- `--out` — output MP4 path. Default `video.mp4` in the current directory.
- `--poll-interval` — seconds between status checks (default 10).
- `--poll-timeout` — max seconds to wait for the task (default 600).

The script prints the path of the written MP4 to stdout.

## Workflow (async)

1. **Create task** — `POST /v1/video_generation` → returns `task_id`
2. **Poll status** — `GET /v1/tasks/{task_id}` → `file_id` on success
3. **Download** — `GET /v1/files/{file_id}/download` → MP4 file
