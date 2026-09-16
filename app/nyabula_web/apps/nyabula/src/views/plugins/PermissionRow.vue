<script setup lang="ts">
/* One permission line: Material path icon + Chinese name + id + switch. */
import { MdSwitch, permissionIcon, permissionZh } from '@nyabula/ui';

defineProps<{ name: string; granted: boolean; disabled?: boolean; busy?: boolean; compact?: boolean }>();
const emit = defineEmits<{ (e: 'toggle', v: boolean): void }>();
</script>

<template>
  <div class="perm-row" :class="{ compact, off: !granted }">
    <span class="perm-icon" :class="{ on: granted }">
      <svg viewBox="0 0 24 24" aria-hidden="true"><path :d="permissionIcon(name)" /></svg>
    </span>
    <span class="perm-text">
      <span class="perm-zh">{{ permissionZh(name) }}</span>
      <span class="perm-id mono">{{ name }}</span>
    </span>
    <MdSwitch :model-value="granted" :disabled="disabled || busy" @update:model-value="emit('toggle', $event)" />
  </div>
</template>

<style scoped>
.perm-row {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 10px 12px;
  border-radius: var(--radius-m);
  background: var(--md-surface-container);
  transition: background var(--dur-short, 0.15s) var(--ease-standard, ease);
}
.perm-row.compact { padding: 8px 10px; gap: 10px; }
.perm-icon {
  width: 36px;
  height: 36px;
  flex: none;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border-radius: var(--radius-full, 999px);
  background: var(--md-surface-container-highest);
  color: var(--md-on-surface-variant);
}
.perm-icon.on { background: var(--md-primary-container); color: var(--md-on-primary-container); }
.perm-icon svg { width: 20px; height: 20px; fill: currentColor; }
.compact .perm-icon { width: 30px; height: 30px; }
.compact .perm-icon svg { width: 17px; height: 17px; }
.perm-text { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
.perm-zh { font-weight: 600; font-size: 14px; color: var(--md-on-surface); }
.perm-id { font-size: 11.5px; color: var(--md-on-surface-variant); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
</style>
