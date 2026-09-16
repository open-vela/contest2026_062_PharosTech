<script setup lang="ts">
/* Four stat cards from /stats/overview. */
import { MdCard, Skeleton, UiIcon } from '@nyabula/ui';
import type { useAccountPage } from './account.logic';

const props = defineProps<{ page: ReturnType<typeof useAccountPage>; compact?: boolean }>();
const p = props.page;
</script>

<template>
  <div class="stats" :class="{ compact }">
    <template v-if="p.loader.busy.value && !p.account.overview">
      <MdCard v-for="i in 4" :key="i"><Skeleton :lines="2" /></MdCard>
    </template>
    <MdCard v-for="c in p.statCards.value" v-else :key="c.id" class="stat">
      <div class="stat-icon"><UiIcon :name="c.icon" :size="20" /></div>
      <div class="stat-body">
        <div class="stat-value">{{ c.value.toLocaleString() }}</div>
        <div class="stat-label">{{ c.label }}</div>
      </div>
    </MdCard>
  </div>
</template>

<style scoped>
.stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; }
.stats.compact { grid-template-columns: repeat(2, 1fr); }
.stat { display: flex; align-items: center; gap: 14px; }
.stat-icon {
  width: 42px;
  height: 42px;
  border-radius: 12px;
  display: grid;
  place-items: center;
  background: var(--md-primary-container);
  color: var(--md-on-primary-container);
  flex: none;
}
.stat-value { font: 700 24px var(--font-title); color: var(--md-on-surface); line-height: 1.1; }
.stat-label { font-size: 12.5px; color: var(--md-on-surface-variant); margin-top: 2px; }
</style>
