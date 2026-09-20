<script setup lang="ts">
/* Phone: full-screen chat; composer sticks to the bottom above the shell
 * bottom area (var(--shell-bottom)) and the safe inset. */
import { MdChip } from '@nyabula/ui';
import ChatThread from './ChatThread.vue';
import ChatComposer from './ChatComposer.vue';
import { useAgentPage } from './agent.logic';

const page = useAgentPage();
</script>

<template>
  <div class="agent-phone">
    <p v-if="page.unsupported.value" class="notice"><span class="contract-only">契约预留</span> 设备未实现 agent.chat</p>
    <ChatThread :messages="page.messages.value" :connected="page.session.connected" class="thread" />
    <div class="dock">
      <div v-if="!page.messages.value.length" class="chips">
        <MdChip v-for="q in page.quickPrompts" :key="q" :disabled="!page.session.connected || !page.session.canControl" @click="page.send(q)">{{ q }}</MdChip>
      </div>
      <ChatComposer :page="page" />
    </div>
  </div>
</template>

<style scoped>
.agent-phone { display: flex; flex-direction: column; min-height: 100%; padding: 8px 12px 0; box-sizing: border-box; }
.notice { font-size: 12.5px; color: var(--md-on-surface-variant); margin: 0 0 6px; display: flex; align-items: center; gap: 6px; }
.thread { flex: 1; padding-bottom: 8px; }
.dock {
  position: sticky;
  bottom: 0;
  margin: 0 -12px;
  padding: 8px 12px calc(var(--shell-bottom) + var(--safe-b));
  background: var(--md-surface);
  border-top: 1px solid var(--md-outline-variant);
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.chips { display: flex; gap: 8px; overflow-x: auto; scrollbar-width: none; }
.chips::-webkit-scrollbar { display: none; }
.chips > * { flex: none; }
</style>
