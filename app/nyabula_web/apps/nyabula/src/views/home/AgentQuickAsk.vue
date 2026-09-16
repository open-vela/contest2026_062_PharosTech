<script setup lang="ts">
/* One-line prompt that jumps into the agent page with the text prefilled. */
import { ref } from 'vue';
import { useRouter } from 'vue-router';
import { MdButton, MdTextField, NyabulaLogo } from '@nyabula/ui';
import type { useHomePage } from './home.logic';

const props = defineProps<{ page: ReturnType<typeof useHomePage> }>();
const router = useRouter();
const text = ref('');
const suggestions = ['今天天气怎么样？', '设个 10 分钟倒计时', '播放点音乐', '你现在心情如何？'];
function send(t = text.value) {
  const q = t.trim();
  if (!q) return;
  void router.push({ name: 'agent', params: { key: props.page.session.deviceKey ?? '' }, query: { q } });
}
</script>

<template>
  <div class="ask">
    <NyabulaLogo :size="40" />
    <div class="ask-body">
      <MdTextField v-model="text" placeholder="问问 Nyabula…" icon="chat" @enter="send()" />
      <div class="sugg">
        <button v-for="s in suggestions" :key="s" class="sugg-chip" @click="send(s)">{{ s }}</button>
      </div>
    </div>
    <MdButton :disabled="!text.trim()" @click="send()">发送</MdButton>
  </div>
</template>

<style scoped>
.ask { display: flex; align-items: flex-start; gap: 12px; padding: 14px 16px; border-radius: var(--radius-l); background: var(--md-surface-container); }
.ask-body { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 8px; }
.sugg { display: flex; flex-wrap: wrap; gap: 6px; }
.sugg-chip { border: none; background: var(--md-surface-container-high); color: var(--md-on-surface-variant); font: 500 12px var(--font-body); padding: 5px 10px; border-radius: 999px; cursor: pointer; }
.sugg-chip:hover { color: var(--md-on-surface); background: var(--md-surface-container-highest); }
</style>
