import vue from '@vitejs/plugin-vue'
import { fileURLToPath, URL } from 'node:url'
import { defineConfig } from 'vite'

// ── Remote Bridge ────────────────────────────────────────────────────
// Set REMOTE_BRIDGE_URL to point Vite at a remote Bridge, e.g.:
//   REMOTE_BRIDGE_URL=https://115.120.249.45:40092  npm run dev
// If the URL already includes a path (e.g. /health), the proxy rewrite
// will NOT double-prefix — it only adds /health when the target is bare.
// Defaults to localhost:8199 for local development.
const REMOTE = process.env.REMOTE_BRIDGE_URL || 'http://localhost:8199'
const REMOTE_WS = REMOTE.replace(/^http/, 'ws')

// When the REMOTE URL already has a path component (e.g. https://x:40092/health),
// the proxy should NOT add /health again via rewrite.
// If REMOTE is just a bare host:port (local dev), add /health prefix.
const _hasPath = new URL(REMOTE).pathname !== '/'
const _rewrite = _hasPath
  ? undefined
  : (path: string) => `/health${path}`

export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },
  server: {
    port: 15173,
    host: '0.0.0.0',
    watch: {
      usePolling: true,
      interval: 500,
    },
    proxy: {
      // SSE chat streaming route — must come before the general /api proxy
      // to ensure streaming responses are not buffered by http-proxy.
      '/api/chat': {
        target: REMOTE,
        changeOrigin: true,
        secure: false,
        rewrite: _rewrite,
        configure: (proxy) => {
          proxy.on('proxyRes', (proxyRes, req, res) => {
            if (proxyRes.headers['content-type']?.includes('text/event-stream')) {
              res.removeHeader('content-length');
              res.setHeader('cache-control', 'no-cache');
              res.setHeader('connection', 'keep-alive');
              res.setHeader('x-accel-buffering', 'no');
            }
          });
        },
      },
      '/api': {
        target: REMOTE,
        changeOrigin: true,
        secure: false,
        rewrite: _rewrite,
      },
      '/ws': {
        target: REMOTE_WS,
        ws: true,
        changeOrigin: true,
        secure: false,
        rewrite: _rewrite,
      },
    },
  },
})
