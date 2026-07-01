<script setup lang="ts">
/**
 * LlmTracePanel
 * ------------------------------------------------------------------
 * Collapsible panel that displays LLM execution traces inline below an
 * assistant message bubble.
 *
 * Props
 * -----
 * - ``traces`` — list of ``LlmTraceSummary`` objects to render.
 *
 * Expand / collapse behaviour is local state so each instance is independent.
 */
import { computed, ref } from 'vue'
import { ArrowRight, Clock, Cpu, Loading } from '@element-plus/icons-vue'
import VueJsonPretty from 'vue-json-pretty'
import 'vue-json-pretty/lib/styles.css'
import type { LlmTraceSummary } from '@/types/agentflow'
import { getLlmTraceDetail } from '@/api/llmTraceClient'
import { useChatStore } from '@/stores/chatStore'

const props = defineProps<{
  traces: LlmTraceSummary[]
}>()

const chatStore = useChatStore()

// ── Local UI state ──────────────────────────────────────────
const expanded = ref(false)
const loadingDetail = ref<string | null>(null)
const detailContent = ref<Record<string, Record<string, unknown>>>({})

// ── Derived ─────────────────────────────────────────────────
const count = computed(() => props.traces.length)
const avgLatency = computed(() => {
  if (count.value === 0) return 0
  const total = props.traces.reduce((s, t) => s + t.latency_ms, 0)
  return Math.round(total / count.value)
})
const totalTokens = computed(() => {
  return props.traces.reduce(
    (s, t) => s + (t.token_usage?.total_tokens ?? 0),
    0,
  )
})
const models = computed(() => {
  const set = new Set(props.traces.map((t) => t.model).filter(Boolean))
  return [...set].join(', ')
})

function toggle(): void {
  expanded.value = !expanded.value
}

async function showDetail(trace: LlmTraceSummary): Promise<void> {
  if (detailContent.value[trace.call_id]) {
    // Already loaded — toggle off
    delete detailContent.value[trace.call_id]
    return
  }

  loadingDetail.value = trace.call_id
  try {
    const detail = await getLlmTraceDetail(
      chatStore.currentAgentId ?? '',
      chatStore.currentUserId ?? '',
      chatStore.currentClientId ?? '',
      trace.call_id,
    )
    detailContent.value = {
      ...detailContent.value,
      [trace.call_id]: {
        request: detail.request as Record<string, unknown>,
        response: detail.response as Record<string, unknown>,
        meta: detail.meta as Record<string, unknown>,
      },
    }
  } catch {
    // Silently fail — detail is best-effort
  } finally {
    loadingDetail.value = null
  }
}

// ── Component registration for VueJsonPretty ──────────────
</script>

<template>
  <div class="llm-trace" :class="{ 'llm-trace--expanded': expanded }">
    <!-- Collapsed trigger bar -->
    <button class="llm-trace__trigger" @click="toggle" :title="expanded ? '收起' : '展开'">
      <span class="llm-trace__trigger-icon">
        <el-icon><Cpu /></el-icon>
      </span>
      <span class="llm-trace__trigger-label">LLM 调用</span>
      <span class="llm-trace__trigger-count">{{ count }} 次</span>
      <span v-if="models" class="llm-trace__trigger-models">{{ models }}</span>
      <span class="llm-trace__trigger-stats">
        <el-icon><Clock /></el-icon>
        {{ avgLatency }}ms
        <template v-if="totalTokens > 0">
          · {{ totalTokens }} tokens
        </template>
      </span>
      <el-icon class="llm-trace__trigger-arrow">
        <ArrowRight />
      </el-icon>
    </button>

    <!-- Expanded content -->
    <div v-if="expanded" class="llm-trace__body">
      <div
        v-for="(trace, idx) in traces"
        :key="trace.call_id"
        class="llm-trace__item"
      >
        <!-- Summary row -->
        <div class="llm-trace__item-header">
          <span class="llm-trace__item-num">#{{ idx + 1 }}</span>
          <span class="llm-trace__item-model">{{ trace.model || '-' }}</span>
          <span class="llm-trace__item-latency">
            <el-icon><Clock /></el-icon>{{ trace.latency_ms }}ms
          </span>
          <span v-if="trace.token_usage" class="llm-trace__item-tokens">
            {{ trace.token_usage.prompt_tokens }} + {{ trace.token_usage.completion_tokens }} = {{ trace.token_usage.total_tokens }}
          </span>
          <el-button
            size="small"
            link
            type="primary"
            :loading="loadingDetail === trace.call_id"
            @click="showDetail(trace)"
          >
            {{ detailContent[trace.call_id] ? '收起详情' : '查看详情' }}
          </el-button>
        </div>

        <!-- Request summary (always visible) -->
        <div class="llm-trace__item-summary">
          <span class="llm-trace__label">req:</span>
          {{ trace.request_summary || '(无)' }}
        </div>
        <div class="llm-trace__item-summary">
          <span class="llm-trace__label">resp:</span>
          {{ trace.response_summary || '(无)' }}
        </div>

        <!-- Expanded detail -->
        <div v-if="detailContent[trace.call_id]" class="llm-trace__item-detail">
          <div class="llm-trace__detail-block">
            <div class="llm-trace__detail-title">Request</div>
            <div class="llm-trace__detail-json">
              <VueJsonPretty
                :data="detailContent[trace.call_id].request"
                :deep="2"
                showLine
                showDoubleQuotes
                :showLength="false"
              />
            </div>
          </div>
          <div class="llm-trace__detail-block">
            <div class="llm-trace__detail-title">Response</div>
            <div class="llm-trace__detail-json">
              <VueJsonPretty
                :data="detailContent[trace.call_id].response"
                :deep="2"
                showLine
                showDoubleQuotes
                :showLength="false"
              />
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
/* ── Container ──────────────────────────────────────────────── */
.llm-trace {
  margin-top: 10px;
  border: 1px solid #e4e7ed;
  border-radius: 8px;
  overflow: hidden;
  background: #fafbfc;
  font-size: 12px;
}

/* ── Collapsed trigger bar ──────────────────────────────────── */
.llm-trace__trigger {
  display: flex;
  align-items: center;
  gap: 6px;
  width: 100%;
  padding: 6px 10px;
  border: none;
  background: transparent;
  cursor: pointer;
  color: #606266;
  font-size: 12px;
  transition: background 0.15s;
}
.llm-trace__trigger:hover {
  background: #f0f2f5;
}
.llm-trace__trigger-icon {
  color: #409eff;
  font-size: 14px;
}
.llm-trace__trigger-label {
  font-weight: 600;
  color: #303133;
  margin-right: 4px;
}
.llm-trace__trigger-count {
  background: #ecf5ff;
  color: #409eff;
  padding: 0 6px;
  border-radius: 4px;
  font-weight: 600;
}
.llm-trace__trigger-models {
  color: #909399;
  margin-left: 4px;
  max-width: 160px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.llm-trace__trigger-stats {
  margin-left: auto;
  display: inline-flex;
  align-items: center;
  gap: 3px;
  color: #909399;
  font-variant-numeric: tabular-nums;
}
.llm-trace__trigger-arrow {
  font-size: 12px;
  transition: transform 0.2s;
  color: #c0c4cc;
}
.llm-trace--expanded .llm-trace__trigger-arrow {
  transform: rotate(90deg);
}

/* ── Body / item list ────────────────────────────────────────── */
.llm-trace__body {
  border-top: 1px solid #ebeef5;
  padding: 4px 0;
}
.llm-trace__item {
  padding: 8px 10px;
  border-bottom: 1px solid #f2f3f5;
}
.llm-trace__item:last-child {
  border-bottom: none;
}
.llm-trace__item-header {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 4px;
}
.llm-trace__item-num {
  font-weight: 600;
  color: #409eff;
  min-width: 24px;
}
.llm-trace__item-model {
  font-weight: 600;
  color: #303133;
  font-size: 13px;
}
.llm-trace__item-latency {
  display: inline-flex;
  align-items: center;
  gap: 2px;
  color: #909399;
  font-variant-numeric: tabular-nums;
}
.llm-trace__item-tokens {
  color: #909399;
  font-variant-numeric: tabular-nums;
  margin-left: auto;
}
.llm-trace__item-summary {
  color: #606266;
  line-height: 1.5;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: pre-wrap;
}
.llm-trace__label {
  color: #c0c4cc;
  margin-right: 4px;
  font-weight: 500;
}

/* ── Detail block (request / response JSON) ──────────────────── */
.llm-trace__item-detail {
  margin-top: 6px;
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.llm-trace__detail-block {
  border: 1px solid #ebeef5;
  border-radius: 4px;
  overflow: hidden;
}
.llm-trace__detail-title {
  background: #f5f7fa;
  padding: 2px 8px;
  font-size: 11px;
  font-weight: 600;
  color: #909399;
  border-bottom: 1px solid #ebeef5;
}
.llm-trace__detail-json {
  padding: 6px 8px;
  background: #ffffff;
  max-height: 300px;
  overflow-y: auto;
  font-size: 11px;
}
/* vue-json-pretty 主题微调 */
.llm-trace__detail-json :deep(.vjs-tree) {
  font-family: 'SF Mono', 'Cascadia Code', 'Consolas', monospace;
  font-size: 11px;
}
.llm-trace__detail-json :deep(.vjs-value) {
  word-break: break-word;
}
.llm-trace__detail-json :deep(.vjs-key) {
  color: #881391;
}
</style>
