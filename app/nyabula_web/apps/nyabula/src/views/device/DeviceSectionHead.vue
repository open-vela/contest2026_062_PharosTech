<script setup lang="ts">
/* Section heading: icon badge, title, the section's one-liner and the
 * device it applies to. */
import { UiIcon } from '@nyabula/ui';
import type { DeviceSection } from './device.logic';

defineProps<{ section: DeviceSection; device?: string }>();
</script>

<template>
  <header class="section-head">
    <span class="badge"><UiIcon :name="section.icon" :size="22" /></span>
    <div class="text">
      <h1 class="page-title">
        {{ section.label }}
        <span v-if="section.extension" class="tag info">扩展</span>
      </h1>
      <p class="page-sub">
        <template v-if="section.desc">{{ section.desc }} · </template>{{ device ?? '设备' }}
      </p>
    </div>
  </header>
</template>

<style scoped>
.section-head { display: flex; align-items: center; gap: 14px; margin-bottom: 18px; }
.badge {
  width: 46px;
  height: 46px;
  flex: none;
  display: grid;
  place-items: center;
  border-radius: var(--radius-m);
  background: var(--md-primary-container);
  color: var(--md-on-primary-container);
}
.text { min-width: 0; }
.page-title { display: flex; align-items: center; gap: 8px; margin: 0; }
.page-sub { margin: 2px 0 0; }
:root[data-skin='instrument'] .badge { border-radius: 3px; background: transparent; color: var(--md-primary); box-shadow: inset 0 0 0 1px var(--md-outline); }
:root[data-skin='nebula'] .badge {
  border-radius: 16px;
  background: radial-gradient(120% 120% at 30% 20%, rgba(var(--md-primary-rgb), 0.34), rgba(var(--md-primary-rgb), 0.08));
  color: var(--md-primary);
  box-shadow: inset 0 1px 0 var(--nb-hi), 0 0 0 1px rgba(var(--md-primary-rgb), 0.2);
}
</style>
