<script setup lang="ts">
/* Title + SegmentedTabs on the right (or stacked full-width when `stacked`). */
import SegmentedTabs, { type SegmentItem } from '../components/SegmentedTabs.vue';

export interface NkSegmentRowProps {
  modelValue: string;
  title: string;
  sub?: string;
  items: SegmentItem[];
  stacked?: boolean;
}
withDefaults(defineProps<NkSegmentRowProps>(), { stacked: false });
const emit = defineEmits<{ (e: 'update:modelValue', v: string): void }>();
</script>

<template>
  <div class="nk-seg-row" :class="{ stacked }">
    <div class="nk-seg-main">
      <div class="nk-seg-title">{{ title }}</div>
      <div v-if="sub" class="nk-seg-sub">{{ sub }}</div>
    </div>
    <SegmentedTabs
      :items="items"
      :model-value="modelValue"
      :stretch="stacked"
      @update:model-value="emit('update:modelValue', $event)"
    />
  </div>
</template>

<style scoped>
.nk-seg-row { display: flex; align-items: center; justify-content: space-between; gap: 12px; min-height: 52px; padding: 8px 12px; color: var(--md-on-surface); }
.nk-seg-row.stacked { flex-direction: column; align-items: stretch; }
.nk-seg-main { min-width: 0; }
.nk-seg-title { font: 600 14px var(--font-body); }
.nk-seg-sub { margin-top: 2px; font: 400 12px var(--font-body); color: var(--md-on-surface-variant); }
</style>
