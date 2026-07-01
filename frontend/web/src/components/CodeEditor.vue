<script setup lang="ts">
/**
 * CodeEditor.vue — CodeMirror 6 代码编辑器组件
 *
 * 自动检测语言模式（JSON / YAML / Markdown / Shell / Python），
 * 支持只读/编辑模式切换，Ctrl+S 保存快捷键，未保存标记。
 */
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { EditorView, keymap, placeholder } from '@codemirror/view'
import { EditorState } from '@codemirror/state'
import { defaultKeymap, indentWithTab } from '@codemirror/commands'
import { syntaxHighlighting, defaultHighlightStyle } from '@codemirror/language'

// Language support
import { json } from '@codemirror/lang-json'
import { yaml } from '@codemirror/lang-yaml'
import { markdown } from '@codemirror/lang-markdown'
import { python } from '@codemirror/lang-python'
import { javascript } from '@codemirror/lang-javascript'

const props = withDefaults(defineProps<{
  modelValue: string
  fileName?: string
  readonly?: boolean
  /** Placeholder text shown when the editor is empty */
  placeholderText?: string
}>(), {
  fileName: '',
  readonly: false,
  placeholderText: '开始输入…',
})

const emit = defineEmits<{
  'update:modelValue': [value: string]
  'save': []
}>()

// ── Editor ref ────────────────────────────────────────────

const editorEl = ref<HTMLDivElement | null>(null)
let view: EditorView | null = null
const isDirty = ref(false)
const currentLang = ref('text')

// ── Language detection ─────────────────────────────────────

function detectLanguage(fileName: string): string {
  const ext = fileName.split('.').pop()?.toLowerCase() ?? ''
  switch (ext) {
    case 'json': case 'jsonc': return 'json'
    case 'yaml': case 'yml': return 'yaml'
    case 'md': case 'markdown': return 'markdown'
    case 'py': return 'python'
    case 'ts': case 'js': case 'tsx': case 'jsx': return 'javascript'
    case 'sh': case 'bash': case 'zsh': return 'shell'
    case 'html': case 'htm': return 'html'
    case 'css': case 'scss': return 'css'
    default: return 'text'
  }
}

function langExtension(lang: string) {
  switch (lang) {
    case 'json': return json()
    case 'yaml': return yaml()
    case 'markdown': return markdown()
    case 'python': return python()
    case 'javascript': return javascript()
    default: return []
  }
}

// ── Build editor ──────────────────────────────────────────

function buildEditor(content: string) {
  const saveKeymap = keymap.of([
    {
      key: 'Mod-s',
      run: () => {
        emit('save')
        isDirty.value = false
        return true
      },
    },
    indentWithTab,
    ...defaultKeymap,
  ])

  const state = EditorState.create({
    doc: content,
    extensions: [
      langExtension(currentLang.value),
      syntaxHighlighting(defaultHighlightStyle),
      saveKeymap,
      EditorView.updateListener.of((update) => {
        if (update.docChanged) {
          const val = update.state.doc.toString()
          emit('update:modelValue', val)
          isDirty.value = val !== props.modelValue
        }
      }),
      EditorView.editable.of(!props.readonly),
      props.placeholderText ? placeholder(props.placeholderText) : [],
      EditorView.lineWrapping,
    ],
  })

  view = new EditorView({
    state,
    parent: editorEl.value!,
  })
}

// ── Lifecycle ─────────────────────────────────────────────

onMounted(() => {
  currentLang.value = detectLanguage(props.fileName)
  buildEditor(props.modelValue)
})

onBeforeUnmount(() => {
  view?.destroy()
  view = null
})

// ── Watch file changes ────────────────────────────────────

watch(() => props.fileName, (newFile) => {
  const newLang = detectLanguage(newFile)
  if (newLang !== currentLang.value) {
    currentLang.value = newLang
    view?.destroy()
    buildEditor(props.modelValue)
  }
})

watch(() => props.modelValue, (newVal) => {
  if (view && newVal !== view.state.doc.toString()) {
    view.dispatch({
      changes: { from: 0, to: view.state.doc.length, insert: newVal },
    })
  }
})

// ── Expose ────────────────────────────────────────────────

defineExpose({ isDirty })
</script>

<template>
  <div class="code-editor" data-testid="code-editor">
    <div v-if="fileName" class="code-editor__header">
      <span class="code-editor__lang-badge">{{ currentLang }}</span>
      <span class="code-editor__filename">{{ fileName }}</span>
      <span v-if="isDirty" class="code-editor__dirty" title="未保存的更改">●</span>
      <span v-if="readonly" class="code-editor__readonly-badge">只读</span>
    </div>
    <div ref="editorEl" class="code-editor__editor"></div>
  </div>
</template>

<style scoped>
.code-editor {
  display: flex;
  flex-direction: column;
  height: 100%;
  border: 1px solid #dcdfe6;
  border-radius: 4px;
  overflow: hidden;
}
.code-editor__header {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 12px;
  background: #f5f7fa;
  border-bottom: 1px solid #e4e7ed;
  font-size: 12px;
}
.code-editor__lang-badge {
  background: #409eff;
  color: #fff;
  padding: 1px 6px;
  border-radius: 3px;
  font-size: 11px;
  font-weight: 600;
  text-transform: uppercase;
}
.code-editor__filename {
  color: #606266;
  font-family: monospace;
}
.code-editor__dirty {
  color: #e6a23c;
  font-size: 14px;
  line-height: 1;
}
.code-editor__readonly-badge {
  color: #909399;
  margin-left: auto;
  font-style: italic;
}
.code-editor__editor {
  flex: 1;
  overflow: auto;
}
.code-editor__editor :deep(.cm-editor) {
  height: 100%;
}
.code-editor__editor :deep(.cm-scroller) {
  font-family: 'JetBrains Mono', 'Fira Code', 'Consolas', monospace;
  font-size: 13px;
}
</style>
