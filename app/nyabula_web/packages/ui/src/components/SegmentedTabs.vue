<script setup lang="ts">
/* Segmented control / tab strip with a sliding indicator. */
import { computed } from 'vue';
import UiIcon from './UiIcon.vue';

export interface SegmentItem {
  id: string;
  label: string;
  icon?: string;
  badge?: number | string;
}
const props = defineProps<{ items: SegmentItem[]; modelValue: string; stretch?: boolean }>();
const emit = defineEmits<{ (e: 'update:modelValue', v: string): void }>();
const idx = computed(() => Math.max(0, props.items.findIndex((i) => i.id === props.modelValue)));
</script>

<template>
  <div class="seg" :class="{ stretch }" role="tablist" :style="{ '--n': items.length, '--i': idx }">
    <span class="indicator" />
    <button
      v-for="it in items"
      :key="it.id"
      class="seg-btn"
      :class="{ active: it.id === modelValue }"
      role="tab"
      :aria-selected="it.id === modelValue"
      @click="emit('update:modelValue', it.id)"
    >
      <UiIcon v-if="it.icon" :name="it.icon" :size="17" />
      <span>{{ it.label }}</span>
      <span v-if="it.badge !== undefined && it.badge !== ''" class="badge">{{ it.badge }}</span>
    </button>
  </div>
</template>

<style scoped>
.seg {
  position: relative;
  display: inline-grid;
  grid-auto-flow: column;
  grid-auto-columns: 1fr;
  background: var(--md-surface-container);
  border-radius: 999px;
  padding: 4px;
  gap: 2px;
}
.seg.stretch { display: grid; width: 100%; }
.indicator {
  position: absolute;
  top: 4px;
  bottom: 4px;
  left: 4px;
  width: calc((100% - 8px) / var(--n));
  border-radius: 999px;
  background: var(--md-secondary-container);
  transform: translateX(calc(var(--i) * 100%));
  transition: transform var(--dur) var(--ease-spring);
}
.seg-btn {
  position: relative;
  z-index: 1;
  border: none;
  background: transparent;
  color: var(--md-on-surface-variant);
  font: 600 13.5px var(--font-body);
  padding: 8px 16px;
  border-radius: 999px;
  cursor: pointer;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  white-space: nowrap;
  transition: color var(--dur-fast);
}
.seg-btn.active { color: var(--md-on-secondary-container); }
.badge {
  min-width: 18px;
  height: 18px;
  padding: 0 5px;
  border-radius: 999px;
  background: var(--md-primary);
  color: var(--md-on-primary);
  font-size: 11px;
  display: inline-grid;
  place-items: center;
}
</style>
