# Provider & key injection (MiniMax)

## Provider

| Provider | Env var | Default model | API endpoint | Status |
|----------|---------|---------------|-------------|--------|
| `minimax` | `API_KEY` | `image-01` | `https://api.minimax.io/v1/image_generation` | ✅ default |

Single provider — no selection needed. `API_KEY` is read directly from
`os.environ["API_KEY"]` at runtime.

## How the API key reaches the sandbox

`generate.py` reads `os.environ["API_KEY"]`. The sandbox drops `*_API_KEY`
from the per-call extra-env path by default, so the key must reach the sandbox
either through:

- **Environment whitelist** — add `API_KEY` to the sandbox's
  `envWhitelist` and export it in the host environment.
- **Vaults injection** (recommended) — manage the key in the Vaults UI and
  declare it on the Environment the session runs in.
