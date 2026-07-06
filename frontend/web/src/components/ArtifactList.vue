<script setup lang="ts">
/**
 * ArtifactList
 * ------------------------------------------------------------------
 * Renders structured artifact metadata (images, HTML reports, file downloads)
 * below an assistant message bubble.
 *
 * Each artifact type gets a distinct visual treatment:
 * - image  → inline thumbnail
 * - html   → "查看报告" button
 * - download → "下载文件" button with filename
 */
import { computed } from 'vue'
import { Download, View } from '@element-plus/icons-vue'
import type { ArtifactInfo } from '../types/agentflow'

const props = defineProps<{
  artifacts: ArtifactInfo[]
}>()

const images = computed(() => props.artifacts.filter((a) => a.type === 'image'))
const nonImages = computed(() => props.artifacts.filter((a) => a.type !== 'image'))

function artifactIcon(type: string): string {
  if (type === 'html') return '📄'
  if (type === 'download') return '📥'
  return '📎'
}
</script>

<template>
  <div v-if="artifacts.length > 0" class="artifact-list" data-testid="artifact-list">
    <!-- Images: inline grid -->
    <div v-if="images.length > 0" class="artifact-list__images">
      <a
        v-for="(img, i) in images"
        :key="i"
        :href="img.url"
        target="_blank"
        rel="noopener noreferrer"
        class="artifact-list__image-link"
        :title="img.name"
      >
        <img
          :src="img.url"
          :alt="img.name"
          class="artifact-list__image"
          loading="lazy"
        />
      </a>
    </div>

    <!-- Non-image artifacts: download/report buttons -->
    <div v-if="nonImages.length > 0" class="artifact-list__files">
      <a
        v-for="(art, i) in nonImages"
        :key="i"
        :href="art.url"
        target="_blank"
        rel="noopener noreferrer"
        class="artifact-list__file"
        :class="`artifact-list__file--${art.type}`"
        :download="art.type === 'download' ? art.name : undefined"
      >
        <span class="artifact-list__file-icon">{{ artifactIcon(art.type) }}</span>
        <span class="artifact-list__file-body">
          <span class="artifact-list__file-name">{{ art.name }}</span>
          <span class="artifact-list__file-action">
            <template v-if="art.type === 'html'">
              <el-button size="small" :icon="View" text>查看报告</el-button>
            </template>
            <template v-else>
              <el-button size="small" :icon="Download" text>下载文件</el-button>
            </template>
          </span>
        </span>
      </a>
    </div>
  </div>
</template>

<style scoped>
.artifact-list {
  margin-top: 10px;
  padding-top: 10px;
  border-top: 1px solid #ebeef5;
}

/* ── Image grid ── */
.artifact-list__images {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-bottom: 8px;
}
.artifact-list__image-link {
  display: block;
  line-height: 0;
  border-radius: 8px;
  overflow: hidden;
  border: 1px solid #e4e7ed;
  transition: box-shadow 0.15s;
}
.artifact-list__image-link:hover {
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.1);
}
.artifact-list__image {
  max-width: 200px;
  max-height: 200px;
  object-fit: contain;
  background: #f5f7fa;
}

/* ── File list ── */
.artifact-list__files {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.artifact-list__file {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 8px 12px;
  border-radius: 8px;
  background: #f8f9fb;
  border: 1px solid #ebeef5;
  text-decoration: none;
  transition: background 0.15s, border-color 0.15s;
}
.artifact-list__file:hover {
  background: #ecf5ff;
  border-color: #d9ecff;
}
.artifact-list__file-icon {
  font-size: 20px;
  line-height: 1;
  flex-shrink: 0;
}
.artifact-list__file-body {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}
.artifact-list__file-name {
  font-size: 13px;
  color: #303133;
  font-weight: 500;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.artifact-list__file-action {
  font-size: 12px;
}
</style>
