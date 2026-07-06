<script setup lang="ts">
/**
 * NewSessionDialog.vue — 新建会话对话框
 *
 * 允许用户输入 client_id 和 user_id 来创建新会话，
 * 而不是自动生成随机值。用户也可以使用自动生成的默认值。
 */
import { ref } from 'vue'

interface Props {
  visible: boolean
  defaultUserId?: string
}

interface Emits {
  (e: 'create', clientId: string, userId: string): void
  (e: 'close'): void
}

const props = withDefaults(defineProps<Props>(), {
  defaultUserId: 'web-user',
})
const emit = defineEmits<Emits>()

const clientId = ref('')
const userId = ref(props.defaultUserId)

/** 生成一个简短的 ID 用于默认填充 */
function generateShortId(): string {
  return 'client_' + Date.now().toString(36) + Math.random().toString(36).slice(2, 8)
}

/** 打开对话框时初始化默认值 */
function onOpen(): void {
  clientId.value = generateShortId()
  userId.value = props.defaultUserId
}

function handleCreate(): void {
  const cid = clientId.value.trim()
  const uid = userId.value.trim()
  if (!cid) {
    clientId.value = generateShortId()
    emit('create', clientId.value, uid || props.defaultUserId)
  } else {
    emit('create', cid, uid || props.defaultUserId)
  }
}

function handleClose(): void {
  emit('close')
}

defineExpose({ clientId, userId, handleCreate, handleClose, onOpen, generateShortId })
</script>

<template>
  <el-dialog
    :model-value="visible"
    title="新建会话"
    width="420px"
    :close-on-click-modal="false"
    @open="onOpen"
    @closed="handleClose"
  >
    <el-form label-width="100px" label-position="top">
      <el-form-item label="Client ID" required>
        <el-input
          v-model="clientId"
          placeholder="留空则自动生成"
          data-testid="new-session-client-id"
        />
        <div class="new-session-dialog__hint">
          会话的唯一标识。留空将自动生成一个随机 ID。
        </div>
      </el-form-item>

      <el-form-item label="User ID" required>
        <el-input
          v-model="userId"
          placeholder="例如: web-user"
          data-testid="new-session-user-id"
        />
        <div class="new-session-dialog__hint">
          用户标识。与 Client ID 一起构成会话的唯一性。
        </div>
      </el-form-item>
    </el-form>

    <template #footer>
      <el-button @click="handleClose">取消</el-button>
      <el-button type="primary" data-testid="new-session-confirm" @click="handleCreate">
        创建
      </el-button>
    </template>
  </el-dialog>
</template>

<style scoped>
.new-session-dialog__hint {
  font-size: 12px;
  color: #909399;
  margin-top: 4px;
  line-height: 1.4;
}
</style>
