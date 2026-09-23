<script setup lang="ts">
/* Big number + unit + label with optional trend arrow and icon. */
import UiIcon from '../components/UiIcon.vue';

export interface NkStatTileProps {
  value: string | number;
  unit?: string;
  label: string;
  icon?: string;
  trend?: 'up' | 'down' | 'flat';
  /** Optional trend caption, e.g. "+3%". */
  trendText?: string;
}
defineProps<NkStatTileProps>();
const TREND_ICON: Record<string, string> = { up: 'trending_up', down: 'trending_down', flat: 'trending_flat' };
</script>

<template>
  <div class="nk-stat">
    <div class="nk-stat-head">
      <UiIcon v-if="icon" :name="icon" :size="18" />
      <span class="nk-stat-label">{{ label }}</span>
    </div>
    <div class="nk-stat-value">
      <span class="nk-stat-num">{{ value }}</span>
      <span v-if="unit" class="nk-stat-unit">{{ unit }}</span>
    </div>
    <div v-if="trend" class="nk-stat-trend" :class="'t-' + trend">
      <UiIcon :name="TREND_ICON[trend]" :size="16" />
      <span v-if="trendText">{{ trendText }}</span>
    </div>
  </div>
</template>

<style scoped>
.nk-stat {
  flex: 1; min-width: 0;
  padding: 14px 16px;
  border-radius: var(--radius-m);
  background: var(--md-surface-container-high);
  color: var(--md-on-surface);
}
.nk-stat-head { display: flex; align-items: center; gap: 6px; color: var(--md-on-surface-variant); font: 600 12px var(--font-body); }
.nk-stat-value { margin-top: 8px; display: flex; align-items: baseline; gap: 4px; }
.nk-stat-num { font: 700 30px var(--font-title); line-height: 1.1; }
.nk-stat-unit { font: 600 13px var(--font-body); color: var(--md-on-surface-variant); }
.nk-stat-trend { margin-top: 6px; display: inline-flex; align-items: center; gap: 4px; font: 600 12px var(--font-body); }
.nk-stat-trend.t-up { color: var(--md-success); }
.nk-stat-trend.t-down { color: var(--md-warning); }
.nk-stat-trend.t-flat { color: var(--md-on-surface-variant); }
</style>
