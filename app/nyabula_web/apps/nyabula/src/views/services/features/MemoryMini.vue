<script setup lang="ts">
/* Memory mini: latest card in one line + "random recall". Shares
 * nyabula.feature.memory with MemoryFeature; recall pushes { text, tag }. */
import { computed, reactive, watch } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory, type FeatureMiniProps } from './contract';

const props = defineProps<FeatureMiniProps>();
const eye = useEyeStore();

interface Card { id: string; text: string; at: number; tag: string }
const TAG_LABEL: Record<string, string> = { daily: '日常', family: '家人', pet: '猫咪', todo: '待办', fun: '趣事' };
const state = reactive(useFeatureMemory(props.type, {
  cards: [
    { id: 'm1', text: '主人喜欢在早上喝黑咖啡，不加糖。', at: Date.now() - 86400e3 * 3, tag: 'daily' },
    { id: 'm2', text: '小花每天晚上八点要吃罐头。', at: Date.now() - 86400e3, tag: 'pet' },
    { id: 'm3', text: '周末全家一起看了电影，大家都很开心。', at: Date.now() - 3600e3 * 5, tag: 'family' },
  ] as Card[],
  selected: 'm1',
}));
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const latest = computed<Card | null>(() => [...state.cards].sort((a, b) => b.at - a.at)[0] ?? null);
const text = computed(() => (props.active && typeof props.payload?.text === 'string' ? props.payload.text : latest.value?.text ?? '还没有记忆'));
const tag = computed(() => (props.active && typeof props.payload?.tag === 'string' ? props.payload.tag : latest.value ? TAG_LABEL[latest.value.tag] ?? latest.value.tag : ''));

function recall(): void {
  if (!state.cards.length) return;
  const pool = state.cards.length > 1 ? state.cards.filter((c) => c.id !== state.selected) : state.cards;
  const c = pool[Math.floor(Math.random() * pool.length)];
  state.selected = c.id;
  void eye.setScene(props.type, eye.sceneStyle, { text: c.text, tag: TAG_LABEL[c.tag] ?? c.tag });
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="mm">
    <div class="mm-text">
      <span v-if="tag" class="mm-tag">{{ tag }}</span>
      <span class="mm-line">{{ text }}</span>
    </div>
    <MdButton variant="tonal" class="mm-btn" :disabled="!state.cards.length" @click="recall"><UiIcon name="auto_awesome" :size="16" /> 随机回顾</MdButton>
    <MdButton v-if="active" variant="icon" aria-label="隐藏" @click="hide"><UiIcon name="close" :size="18" /></MdButton>
  </div>
</template>

<style scoped>
.mm { display: flex; align-items: center; gap: 8px; min-height: 44px; }
.mm-text { flex: 1; min-width: 0; display: flex; align-items: center; gap: 6px; }
.mm-tag { flex: none; font: 600 11px var(--font-body); padding: 2px 7px; border-radius: 999px; background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.mm-line { font-size: 13.5px; color: var(--md-on-surface); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.mm-btn { flex: none; min-height: 40px; padding: 0 14px; display: inline-flex; align-items: center; gap: 4px; }
</style>
