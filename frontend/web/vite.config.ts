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
