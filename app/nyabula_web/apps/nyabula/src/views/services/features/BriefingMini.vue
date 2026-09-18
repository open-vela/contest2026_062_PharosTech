<script setup lang="ts">
import { computed, onMounted } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';
import { useBriefingStore } from '../../../stores/briefing';
import type { FeatureMiniProps } from './contract';
defineProps<FeatureMiniProps>();
const briefing = useBriefingStore();
onMounted(() => void briefing.refresh());
const items = computed(() => briefing.state?.items ?? []);
const current = computed(() => items.value[briefing.state?.index ?? 0] ?? null);
</script>
<template><div class="brief-mini">
  <span>{{ current?.title ?? '尚未生成简报' }}<small v-if="items.length"> {{ (briefing.state?.index ?? 0)+1 }}/{{ items.length }}</small></span>
  <MdButton variant="icon" :disabled="!items.length" :aria-label="briefing.state?.playing ? '停止' : '显示简报'"
    @click="briefing.act(briefing.state?.playing ? 'stop' : 'start')"><UiIcon :name="briefing.state?.playing ? 'stop' : 'play_arrow'" :size="22" /></MdButton>
  <MdButton variant="icon" :disabled="!items.length" aria-label="下一条" @click="briefing.act('next')"><UiIcon name="skip_next" :size="20" /></MdButton>
</div></template>
<style scoped>.brief-mini { display:flex; align-items:center; gap:8px; }.brief-mini>span { flex:1; min-width:0; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }small { color:var(--md-on-surface-variant); }</style>
