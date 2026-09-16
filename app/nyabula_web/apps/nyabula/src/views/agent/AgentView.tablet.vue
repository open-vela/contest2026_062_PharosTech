<script setup lang="ts">
/* Tablet: same centered flow as desktop, no side column; quick prompts as a
 * horizontal chip strip above the composer. */
import { MdButton, MdChip, UiIcon } from '@nyabula/ui';
import ChatThread from './ChatThread.vue';
import ChatComposer from './ChatComposer.vue';
import { useAgentPage } from './agent.logic';

const page = useAgentPage();
</script>

<template>
  <div class="agent-tablet">
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
      <div class="chips">
        <MdChip v-for="q in page.quickPrompts" :key="q" :disabled="!page.session.connected || !page.session.canControl" @click="page.send(q)">{{ q }}</MdChip>
      </div>
      <ChatComposer :page="page" />
    </div>
  </div>
</template>

<style scoped>
.agent-tablet { display: flex; justify-content: center; padding: 18px 20px 24px; height: 100%; box-sizing: border-box; }
.col { width: min(100%, 820px); display: flex; flex-direction: column; gap: 12px; min-height: 0; }
.head { flex: none; }
.thread { flex: 1; min-height: 280px; }
.chips { display: flex; gap: 8px; overflow-x: auto; scrollbar-width: none; padding-bottom: 2px; }
.chips::-webkit-scrollbar { display: none; }
.chips > * { flex: none; }
</style>
