<script setup lang="ts">
/* Desktop: centered conversation column; quick prompts + notes in the
 * shell context panel. */
import { MdButton, MdCard, MdChip, UiIcon } from '@nyabula/ui';
import ContextSlot from '../../components/ContextSlot.vue';
import ChatThread from './ChatThread.vue';
import ChatComposer from './ChatComposer.vue';
import { useAgentPage } from './agent.logic';

const page = useAgentPage();
</script>

<template>
  <div class="agent-desktop">
    <div class="col">
      <div class="row between head">
        <div>
          <h1 class="page-title">对话</h1>
          <p class="page-sub" style="margin: 0">
            agent.chat → agent.delta 流式回复
            <span v-if="page.unsupported.value" class="contract-only" style="margin-left: 6px">契约预留</span>
          </p>
        </div>
        <MdButton variant="text" :disabled="!page.messages.value.length" @click="page.clear()"><UiIcon name="delete" :size="16" /> 清空</MdButton>
      </div>
      <ChatThread :messages="page.messages.value" :connected="page.session.connected" class="thread" />
      <ChatComposer :page="page" />
    </div>

    <ContextSlot>
      <MdCard title="快捷提问">
        <div class="row wrap" style="gap: 8px">
          <MdChip v-for="q in page.quickPrompts" :key="q" :disabled="!page.session.connected || !page.session.canControl" @click="page.send(q)">{{ q }}</MdChip>
        </div>
      </MdCard>
      <MdCard title="说明" style="margin-top: 14px">
        <dl class="kv">
          <div><dt>连接</dt><dd>{{ page.session.connected ? '已连接' : '未连接' }}</dd></div>
          <div><dt>角色</dt><dd>{{ page.session.role ?? '—' }}</dd></div>
          <div><dt>唤醒词</dt><dd>你好，openvela</dd></div>
        </dl>
        <p v-if="page.unsupported.value" class="muted" style="font-size: 12.5px; margin: 10px 0 0">当前设备固件尚未实现 agent.chat（模拟器亦未实现）；契约已定，接入后此页无需改动。</p>
      </MdCard>
    </ContextSlot>
  </div>
</template>

<style scoped>
.agent-desktop { display: flex; justify-content: center; padding: 20px 24px 28px; height: 100%; box-sizing: border-box; }
.col { width: min(100%, 860px); display: flex; flex-direction: column; gap: 12px; min-height: 0; }
.head { flex: none; }
.thread { flex: 1; min-height: 320px; }
</style>
