<script setup lang="ts">
/* Scrollable message list; auto-scrolls to the newest message. */
import { nextTick, ref, watch } from 'vue';
import { EmptyState, UiIcon, NyabulaLogo } from '@nyabula/ui';
import type { ChatMessage } from './agent.logic';

const props = defineProps<{ messages: ChatMessage[]; connected: boolean }>();
const el = ref<HTMLElement | null>(null);

watch(
  () => props.messages.map((m) => m.text.length + (m.pending ? 1 : 0)),
  async () => {
    await nextTick();
    if (el.value) el.value.scrollTop = el.value.scrollHeight;
  },
  { deep: true },
);
</script>

<template>
  <div ref="el" class="thread">
    <EmptyState
      v-if="!messages.length"
      icon="chat"
      title="和 Nyabot 聊聊"
      :hint="connected ? '会话和执行记录由设备保存。配置可用模型后开始对话。' : '设备未连接，当前显示上次读取的记录。'"
      compact
    />
    <div v-for="m in messages" :key="m.id" class="msg" :class="[m.role, { unsupported: m.unsupported, error: !!m.error }]">
      <div v-if="m.role === 'agent'" class="avatar"><NyabulaLogo  :size="18" /></div>
      <div class="bubble">
        <span v-if="m.text">{{ m.text }}</span>
        <span v-if="m.pending" class="pending-label" role="status"> · 等待设备</span>
        <span v-if="m.unsupported" class="contract-only" style="margin-left: 8px">契约预留</span>
      </div>
    </div>
  </div>
</template>

<style scoped>
.thread { display: flex; flex-direction: column; gap: 10px; overflow-y: auto; padding: 4px 2px; scroll-behavior: smooth; }
.msg { display: flex; gap: 10px; align-items: flex-end; max-width: 100%; }
.msg.user { justify-content: flex-end; }
.avatar {
  width: 30px;
  height: 30px;
  border-radius: 50%;
  display: grid;
  place-items: center;
  background: var(--md-primary-container);
  color: var(--md-on-primary-container);
  flex: none;
}
.bubble {
  max-width: min(78%, 640px);
  padding: 10px 14px;
  border-radius: var(--radius-l);
  font-size: 14.5px;
  line-height: 1.5;
  white-space: pre-wrap;
  word-break: break-word;
  background: var(--md-surface-container);
  color: var(--md-on-surface);
  border-bottom-left-radius: var(--radius-s);
}
.user .bubble {
  background: var(--md-primary);
  color: var(--md-on-primary);
  border-bottom-left-radius: var(--radius-l);
  border-bottom-right-radius: var(--radius-s);
}
.unsupported .bubble { border: 1px dashed var(--md-outline); background: transparent; color: var(--md-on-surface-variant); }
.error .bubble { background: color-mix(in srgb, var(--md-error) 14%, transparent); color: var(--md-error); }
.cursor {
  display: inline-block;
  width: 7px;
  height: 14px;
  margin-left: 2px;
  vertical-align: -2px;
  background: currentColor;
  opacity: 0.6;
  border-radius: 2px;
  animation: blink 1s steps(2, start) infinite;
}
@keyframes blink { to { visibility: hidden; } }
</style>
