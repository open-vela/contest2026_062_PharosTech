<script setup lang="ts">
/* Plugin summary card (desktop grid / tablet portrait). */
import { computed } from 'vue';
import { UiIcon } from '@nyabula/ui';
import type { PluginInfo } from '../../stores/plugins';
import { permSummary, stateLabel, stateTone } from './plugins.logic';

const props = defineProps<{ plugin: PluginInfo; active?: boolean }>();
const emit = defineEmits<{ (e: 'open'): void }>();
const perms = computed(() => permSummary(props.plugin));
</script>

<template>
  <button class="plugin-card" :class="{ active }" @click="emit('open')">
    <span class="icon"><UiIcon :name="plugin.icon ?? 'extension'" :size="26" /></span>
    <span class="body">
      <span class="name">{{ plugin.name }}</span>
      <span class="meta">
        <span v-if="plugin.version" class="mono">v{{ plugin.version }}</span>
        <span class="tag" :class="stateTone(plugin.state)">{{ stateLabel(plugin.state) }}</span>
      </span>
      <span class="perm" :class="{ warn: perms.missing > 0 }">
        <UiIcon name="shield" :size="14" />
        <template v-if="perms.total">已授 {{ perms.granted }}/{{ perms.total }}</template>
        <template v-else>无需权限</template>
      </span>
    </span>
    <UiIcon name="chevron_right" :size="18" class="chev" />
  </button>
</template>

<style scoped>
.plugin-card {
  display: flex;
  align-items: flex-start;
  gap: 14px;
  width: 100%;
  text-align: left;
  padding: 16px;
  border-radius: var(--radius-l, 18px);
  border: 1px solid var(--md-outline-variant);
  background: var(--md-surface-container-low);
  color: var(--md-on-surface);
  cursor: pointer;
  transition: background var(--dur-short, 0.15s) var(--ease-standard, ease), transform var(--dur-short, 0.15s) var(--ease-standard, ease);
}
.plugin-card:hover { background: var(--md-surface-container-high); }
.plugin-card:active { transform: scale(0.99); }
.plugin-card.active { border-color: var(--md-primary); background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.icon {
  width: 48px;
  height: 48px;
  flex: none;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border-radius: var(--radius-m);
  background: var(--md-primary-container);
  color: var(--md-on-primary-container);
}
.body { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 6px; }
.name { font: 600 15px var(--font-body); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.meta { display: flex; align-items: center; gap: 8px; font-size: 12px; color: var(--md-on-surface-variant); }
.perm { display: inline-flex; align-items: center; gap: 5px; font-size: 12.5px; color: var(--md-on-surface-variant); }
.perm.warn { color: var(--md-warning); }
.chev { color: var(--md-on-surface-variant); margin-top: 14px; }
</style>
