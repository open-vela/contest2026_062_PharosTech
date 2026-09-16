<script setup lang="ts">
/* Text input + send button. Enter sends; disabled while offline / sending. */
import { MdButton, MdTextField, UiIcon } from '@nyabula/ui';
import type { useAgentPage } from './agent.logic';

const props = defineProps<{ page: ReturnType<typeof useAgentPage> }>();
const p = props.page;

function placeholder(): string {
  if (!p.session.connected) return '设备未连接';
  if (!p.session.isOwner) return '当前需要主人权限';
  if (!p.bot.status?.configured) return '先配置可用模型，也可以保留草稿';
  return '对 Nyabot 说点什么…';
}
</script>

<template>
  <form class="composer" @submit.prevent="p.send()">
    <MdTextField v-model="p.draft.value" :placeholder="placeholder()" :disabled="!p.session.isOwner" label="消息" icon="chat" autocomplete="off" class="field" @enter="p.send()" />
    <MdButton :disabled="!p.canSend.value" class="send" aria-label="发送"><UiIcon name="send" :size="18" /><span class="send-label">发送</span></MdButton>
  </form>
</template>

<style scoped>
.composer { display: flex; gap: 8px; align-items: center; }
.field { flex: 1; min-width: 0; }
.send { flex: none; }
@media (max-width: 420px) { .send-label { display: none; } }
</style>
