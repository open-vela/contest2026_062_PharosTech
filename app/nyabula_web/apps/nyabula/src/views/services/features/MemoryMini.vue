<script setup lang="ts">
/* Memory mini: one memory in a line + "random recall" onto the eyes. Memories
 * are the Core records the full page edits; nothing is kept in the browser. */
import { computed, ref } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';
import { useProductRecords } from '../../../composables/useProductRecords';
import { useEyeScene } from '../../../composables/useEyeScene';
import { memoryScene } from '../../../composables/eyeScenePayload';
import type { ProductRecord } from '../../../stores/product';
import type { FeatureMiniProps } from './contract';

const props = defineProps<FeatureMiniProps>();

interface Card extends ProductRecord { id: string; text: string; at?: number; tag?: string }
const TAG_LABEL: Record<string, string> = { daily: '日常', family: '家人', pet: '猫咪', todo: '待办', fun: '趣事' };
const core = useProductRecords<Card>('memory', 5000);
const cards = computed(() => core.items.value);
const picked = ref('');
const latest = computed<Card | null>(() => [...cards.value].sort((a, b) => (b.at ?? 0) - (a.at ?? 0))[0] ?? null);
const card = computed<Card | null>(() => cards.value.find((c) => c.id === picked.value) ?? latest.value);
const tagOf = (c: Card): string => TAG_LABEL[c.tag ?? 'daily'] ?? c.tag ?? '日常';
const text = computed(() => card.value?.text ?? (core.available.value ? '还没有记忆' : '未连接支持记忆的 Core'));
const tag = computed(() => (card.value ? tagOf(card.value) : ''));

const scene = useEyeScene(props.type, () => (card.value ? memoryScene({ text: card.value.text, tag: tagOf(card.value) }) : null), { hideWhenEmpty: true });

function recall(): void {
  if (!cards.value.length) return;
  const pool = cards.value.length > 1 ? cards.value.filter((c) => c.id !== card.value?.id) : cards.value;
  picked.value = pool[Math.floor(Math.random() * pool.length)]!.id;
  // A held scene follows the pick through its payload; otherwise show it now.
  if (!scene.held.value) void scene.show();
}
</script>

<template>
  <div class="mm">
    <div class="mm-text">
      <span v-if="tag" class="mm-tag">{{ tag }}</span>
      <span class="mm-line">{{ text }}</span>
    </div>
    <MdButton variant="tonal" class="mm-btn" :disabled="!scene.canShow.value" @click="recall"><UiIcon name="auto_awesome" :size="16" /> 随机回顾</MdButton>
    <MdButton v-if="active || scene.held.value" variant="icon" aria-label="隐藏" @click="scene.hide()"><UiIcon name="close" :size="18" /></MdButton>
  </div>
</template>

<style scoped>
.mm { display: flex; align-items: center; gap: 8px; min-height: 44px; }
.mm-text { flex: 1; min-width: 0; display: flex; align-items: center; gap: 6px; }
.mm-tag { flex: none; font: 600 11px var(--font-body); padding: 2px 7px; border-radius: 999px; background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.mm-line { font-size: 13.5px; color: var(--md-on-surface); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.mm-btn { flex: none; min-height: 40px; padding: 0 14px; display: inline-flex; align-items: center; gap: 4px; }
</style>
