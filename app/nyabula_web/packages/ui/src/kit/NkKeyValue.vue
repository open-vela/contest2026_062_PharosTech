<script setup lang="ts">
/* Key/value list. `mono` renders values in monospace (ids, addresses). */
export interface NkKeyValueItem {
  key: string;
  value: string | number;
  mono?: boolean;
  tone?: 'default' | 'primary' | 'ok' | 'warn' | 'error';
}
export interface NkKeyValueProps {
  items: NkKeyValueItem[];
  /** Two-column grid instead of a stacked list. */
  columns?: 1 | 2;
}
withDefaults(defineProps<NkKeyValueProps>(), { columns: 1 });
</script>

<template>
  <dl class="nk-kv" :class="'cols-' + columns">
    <div v-for="(it, i) in items" :key="i" class="nk-kv-item">
      <dt class="nk-kv-key">{{ it.key }}</dt>
      <dd class="nk-kv-val" :class="[{ mono: it.mono }, 'tone-' + (it.tone || 'default')]">{{ it.value }}</dd>
    </div>
  </dl>
</template>

<style scoped>
.nk-kv { margin: 0; display: grid; gap: 2px; }
.nk-kv.cols-2 { grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 2px 12px; }
.nk-kv-item { display: flex; justify-content: space-between; align-items: baseline; gap: 12px; min-height: 40px; padding: 8px 12px; border-radius: var(--radius-s); }
.nk-kv-item:nth-child(odd) { background: var(--md-surface-container-high); }
.nk-kv-key { font: 500 13px var(--font-body); color: var(--md-on-surface-variant); flex: none; }
.nk-kv-val { margin: 0; font: 600 13px var(--font-body); color: var(--md-on-surface); text-align: right; overflow-wrap: anywhere; }
.nk-kv-val.mono { font-family: monospace; }
.nk-kv-val.tone-primary { color: var(--md-primary); }
.nk-kv-val.tone-ok { color: var(--md-success); }
.nk-kv-val.tone-warn { color: var(--md-warning); }
.nk-kv-val.tone-error { color: var(--md-error); }
</style>
