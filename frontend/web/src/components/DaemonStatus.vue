<script setup lang="ts">
import { computed } from 'vue'
import { useDaemonStore } from '../stores/daemonStore'

const daemonStore = useDaemonStore()

const label = computed(() => (daemonStore.isConnected ? '守护进程已连接' : '守护进程已断开'))
</script>

<template>
  <div class="daemon-status" data-testid="daemon-status">
    <span class="dot" :class="{ 'dot--on': daemonStore.isConnected }" aria-hidden="true" />
    <span class="label">{{ label }}</span>
  </div>
</template>

<style scoped>
.daemon-status {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 4px 12px;
  border-radius: 999px;
  background: rgba(255, 255, 255, 0.08);
  font-size: 13px;
  line-height: 1;
  user-select: none;
}
.dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #f56c6c;
  box-shadow: 0 0 0 2px rgba(245, 108, 108, 0.18);
  transition: background-color 0.2s ease, box-shadow 0.2s ease;
}
.dot--on {
  background: #67c23a;
  box-shadow: 0 0 0 2px rgba(103, 194, 58, 0.22);
}
.label {
  color: #303133;
}
</style>
