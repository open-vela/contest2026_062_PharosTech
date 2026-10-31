<script setup lang="ts">
/* Grouped section directory shared by the desktop and landscape tablet
 * variants. Styling comes from the global .side-nav (skinned per theme). */
import { UiIcon } from '@nyabula/ui';
import type { useDevicePage } from './device.logic';

defineProps<{ page: ReturnType<typeof useDevicePage>; compact?: boolean }>();
</script>

<template>
  <nav class="side-nav device-nav" aria-label="设备设置目录">
    <template v-for="g in page.groups.value" :key="g.id">
      <p class="side-nav-group">{{ g.label }}</p>
      <button
        v-for="s in g.items"
        :key="s.id"
        class="side-nav-item"
        :class="{ on: page.current.value.id === s.id }"
        :aria-current="page.current.value.id === s.id ? 'page' : undefined"
        @click="page.go(s.id)"
      >
        <UiIcon :name="s.icon" :size="20" />
        <span class="side-nav-label">
          {{ s.label }}
          <small v-if="!compact && s.desc" class="side-nav-sub">{{ s.desc }}</small>
        </span>
      </button>
    </template>
  </nav>
</template>

<style scoped>
.device-nav { position: sticky; top: 16px; align-self: start; }
</style>
