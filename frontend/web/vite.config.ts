import vue from '@vitejs/plugin-vue'
import { fileURLToPath, URL } from 'node:url'
import { defineConfig } from 'vite'

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
        target: 'http://localhost:8199',
        changeOrigin: true,
        rewrite: (path) => `/health${path}`,
        configure: (proxy) => {
          proxy.on('proxyRes', (proxyRes, req, res) => {
            // For SSE streaming responses, ensure the proxy does not buffer and
            // the browser receives events incrementally via chunked transfer encoding.
            if (proxyRes.headers['content-type']?.includes('text/event-stream')) {
              // Remove Content-Length from the OUTGOING response (res) so the
              // proxy uses chunked transfer encoding. Modifying proxyRes.headers
              // is too late — http-proxy has already copied them to res.
              res.removeHeader('content-length');
              res.setHeader('cache-control', 'no-cache');
              res.setHeader('connection', 'keep-alive');
              res.setHeader('x-accel-buffering', 'no');
            }
          });
        },
      },
      '/api': {
        target: 'http://localhost:8199',
        changeOrigin: true,
        rewrite: (path) => `/health${path}`,
      },
      '/ws': {
        target: 'ws://localhost:8199',
        ws: true,
        changeOrigin: true,
        rewrite: (path) => `/health${path}`,
      },
    },
  },
})
