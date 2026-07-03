<script setup lang="ts">
/**
 * MarkdownContent
 * ------------------------------------------------------------------
 * Renders message content with:
 *  - Full Markdown support via markdown-it
 *  - Code syntax highlighting via highlight.js
 *  - Collapsible <think>...</think> blocks
 */
import { computed, ref } from 'vue'
import MarkdownIt from 'markdown-it'
import hljs from 'highlight.js'

const props = defineProps<{
  content: string
}>()

// ── markdown-it instance ────────────────────────────────────────────────
const md = new MarkdownIt({
  html: true,
  linkify: true,
  typographer: true,
  highlight(str: string, lang: string): string {
    if (lang && hljs.getLanguage(lang)) {
      try {
        return (
          '<pre class="hljs"><code>' +
          hljs.highlight(str, { language: lang, ignoreIllegals: true }).value +
          '</code></pre>'
        )
      } catch {
        // fall through to escaped output
      }
    }
    return '<pre class="hljs"><code>' + md.utils.escapeHtml(str) + '</code></pre>'
  },
})

// ── Segment: think block or regular markdown ────────────────────────────
interface Segment {
  type: 'think' | 'markdown'
  content: string
  html: string
  /** Unique id for the collapse toggle */
  uid: number
}

let segUid = 0
const segments = computed<Segment[]>(() => {
  const segs: Segment[] = []
  // Match <think>...</think> (with optional whitespace around tags)
  const re = /<think>([\s\S]*?)<\/think>/g
  let last = 0
  let m: RegExpExecArray | null

  while ((m = re.exec(props.content)) !== null) {
    // Text before this think block
    if (m.index > last) {
      const raw = props.content.slice(last, m.index)
      segs.push({ type: 'markdown', content: raw, html: md.render(raw), uid: ++segUid })
    }
    // Think block itself
    const thinkContent = m[1].trim()
    segs.push({
      type: 'think',
      content: thinkContent,
      html: md.render(thinkContent),
      uid: ++segUid,
    })
    last = m.index + m[0].length
  }

  // Trailing text after the last think block
  if (last < props.content.length) {
    const raw = props.content.slice(last)
    segs.push({ type: 'markdown', content: raw, html: md.render(raw), uid: ++segUid })
  }

  // If no think blocks at all, treat everything as one markdown segment
  if (segs.length === 0) {
    segs.push({ type: 'markdown', content: props.content, html: md.render(props.content), uid: ++segUid })
  }

  return segs
})

// ── Collapse state per think block ──────────────────────────────────────
const collapsedMap = ref<Record<number, boolean>>({})

function toggleThink(uid: number): void {
  collapsedMap.value[uid] = !collapsedMap.value[uid]
}

function isThinkCollapsed(uid: number): boolean {
  // Default to collapsed (not yet set === true)
  return collapsedMap.value[uid] !== false
}
</script>

<template>
  <div class="markdown-content">
    <template v-for="seg in segments" :key="seg.uid">
      <!-- Think block: collapsible -->
      <div v-if="seg.type === 'think'" class="think-block">
        <button
          class="think-toggle"
          :class="{ 'think-toggle--open': !isThinkCollapsed(seg.uid) }"
          @click="toggleThink(seg.uid)"
        >
          <span class="think-toggle__icon">{{ isThinkCollapsed(seg.uid) ? '▶' : '▼' }}</span>
          <span class="think-toggle__label">思考过程</span>
          <span class="think-toggle__badge">{{ seg.content.length }} 字符</span>
        </button>
        <div
          v-show="!isThinkCollapsed(seg.uid)"
          class="think-body"
          v-html="seg.html"
        />
      </div>

      <!-- Regular markdown -->
      <div v-else class="md-body" v-html="seg.html" />
    </template>
  </div>
</template>

<style scoped>
/* ── Reset spacing for markdown output ──────────────────────────────── */
.markdown-content {
  overflow-wrap: break-word;
}
.markdown-content :deep(p:first-child) {
  margin-top: 0;
}
.markdown-content :deep(p:last-child) {
  margin-bottom: 0;
}
.markdown-content :deep(p) {
  margin: 0.5em 0;
  line-height: 1.6;
}

/* Headings */
.markdown-content :deep(h1),
.markdown-content :deep(h2),
.markdown-content :deep(h3),
.markdown-content :deep(h4) {
  margin: 0.8em 0 0.4em;
  font-weight: 600;
  color: #1a1a1a;
}
.markdown-content :deep(h1) { font-size: 1.3em; }
.markdown-content :deep(h2) { font-size: 1.15em; }
.markdown-content :deep(h3) { font-size: 1.05em; }

/* Lists */
.markdown-content :deep(ul),
.markdown-content :deep(ol) {
  padding-left: 1.5em;
  margin: 0.4em 0;
}
.markdown-content :deep(li) {
  margin: 0.2em 0;
}

/* Inline code */
.markdown-content :deep(code:not(.hljs code)) {
  background: #f0f2f5;
  padding: 2px 6px;
  border-radius: 4px;
  font-size: 0.9em;
  color: #d63384;
}

/* Code blocks (highlight.js) */
.markdown-content :deep(pre) {
  margin: 0.6em 0;
  border-radius: 6px;
  overflow-x: auto;
  overflow-y: hidden;
}
.markdown-content :deep(pre code.hljs) {
  padding: 12px 16px;
  font-size: 13px;
  line-height: 1.5;
  background: #f6f8fa;
  border: 1px solid #e1e4e8;
  border-radius: 6px;
  white-space: pre-wrap;
  word-break: break-word;
  overflow-wrap: break-word;
}

/* Blockquotes */
.markdown-content :deep(blockquote) {
  margin: 0.5em 0;
  padding: 4px 12px;
  border-left: 3px solid #409eff;
  background: #f0f7ff;
  color: #555;
}

/* Tables */
.markdown-content :deep(table) {
  border-collapse: collapse;
  margin: 0.6em 0;
  width: 100%;
  font-size: 13px;
  table-layout: fixed;
}
.markdown-content :deep(th),
.markdown-content :deep(td) {
  border: 1px solid #e0e0e0;
  padding: 6px 10px;
  text-align: left;
}
.markdown-content :deep(th) {
  background: #f5f7fa;
  font-weight: 600;
}

/* Links */
.markdown-content :deep(a) {
  color: #409eff;
  text-decoration: none;
}
.markdown-content :deep(a:hover) {
  text-decoration: underline;
}

.markdown-content :deep(img) {
  display: block;
  max-width: 100%;
  height: auto;
  margin: 12px 0;
  border-radius: 10px;
  object-fit: contain;
}

/* Horizontal rule */
.markdown-content :deep(hr) {
  border: none;
  border-top: 1px solid #e0e0e0;
  margin: 0.8em 0;
}

/* ── Think block collapsible ─────────────────────────────────────────── */
.think-block {
  margin: 0.5em 0;
  border: 1px solid #e8e8e8;
  border-radius: 8px;
  overflow: hidden;
  background: #fafbfc;
}

.think-toggle {
  display: flex;
  align-items: center;
  gap: 8px;
  width: 100%;
  padding: 8px 12px;
  border: none;
  background: #f0f2f5;
  cursor: pointer;
  font-size: 13px;
  color: #606266;
  transition: background 0.15s;
}
.think-toggle:hover {
  background: #e4e7eb;
}

.think-toggle__icon {
  font-size: 10px;
  color: #909399;
  transition: transform 0.2s;
}
.think-toggle--open .think-toggle__icon {
  /* ▼ is already pointing down when open */
}

.think-toggle__label {
  font-weight: 500;
}

.think-toggle__badge {
  margin-left: auto;
  font-size: 11px;
  color: #909399;
  background: #e4e7eb;
  padding: 1px 8px;
  border-radius: 10px;
}

.think-body {
  padding: 8px 14px 12px;
  font-size: 14px;
  line-height: 1.6;
  color: #555;
  border-top: 1px solid #e8e8e8;
}

/* First think block top border removed when toggle is open */
.think-toggle--open + .think-body {
  border-top-color: #e0e0e0;
}

/* Nested think blocks in markdown output should not be processed again */
.think-body :deep(.think-block) {
  /* just show as normal content inside think body */
}
</style>
