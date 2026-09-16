<script setup lang="ts">
import { UiIcon } from '@nyabula/ui';
import type { useHomePage } from './home.logic';

const props = defineProps<{ page: ReturnType<typeof useHomePage>; columns?: number; horizontal?: boolean }>();
const { quickActions } = props.page;
</script>

<template>
  <div class="qa" :class="{ horizontal }" :style="columns ? { gridTemplateColumns: `repeat(${columns}, 1fr)` } : undefined">
    <button v-for="a in quickActions" :key="a.id" class="qa-item hover-lift" @click="a.run()">
      <span class="qa-icon"><UiIcon :name="a.icon" :size="22" /></span>
      <span class="qa-text">
        <span class="qa-label">{{ a.label }}</span>
        <span v-if="a.hint" class="qa-hint">{{ a.hint }}</span>
      </span>
    </button>
  </div>
</template>

<style scoped>
.qa { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 10px; }
.qa.horizontal { display: flex; overflow-x: auto; scrollbar-width: none; padding-bottom: 2px; }
.qa.horizontal::-webkit-scrollbar { display: none; }
.qa.horizontal .qa-item { flex: none; min-width: 132px; }
.qa-item {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 12px;
  border-radius: var(--radius-m);
  border: none;
  background: var(--md-surface-container);
  color: var(--md-on-surface);
  cursor: pointer;
  text-align: left;
  font: inherit;
}
.qa-icon { width: 40px; height: 40px; border-radius: 12px; display: grid; place-items: center; background: var(--md-primary-container); color: var(--md-on-primary-container); flex: none; }
.qa-text { display: flex; flex-direction: column; min-width: 0; }
.qa-label { font: 600 14px var(--font-body); }
.qa-hint { font-size: 12px; color: var(--md-on-surface-variant); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
</style>
