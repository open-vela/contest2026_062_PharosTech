<script setup lang="ts">
/* Lightweight inline-SVG bar chart (no chart library).
 * Renders one bar per item; hover shows a title tooltip. */
import { computed } from 'vue';

const props = defineProps<{
  items: { label: string; value: number }[];
  /** Formats the value for the tooltip / axis, e.g. seconds -> "1.2h". */
  format?: (v: number) => string;
  color?: string;
}>();

const W = 320;
const H = 120;
const PAD_BOTTOM = 18;

const max = computed(() => Math.max(1, ...props.items.map((i) => i.value)));

const bars = computed(() => {
  const n = props.items.length || 1;
  const slot = W / n;
  const bw = Math.min(26, slot * 0.6);
  return props.items.map((it, i) => {
    const h = (it.value / max.value) * (H - PAD_BOTTOM - 6);
    return {
      x: i * slot + (slot - bw) / 2,
      y: H - PAD_BOTTOM - h,
      w: bw,
      h: Math.max(it.value > 0 ? 2 : 0, h),
      label: it.label,
      value: it.value,
    };
  });
});

function fmt(v: number): string {
  return props.format ? props.format(v) : String(v);
}
</script>

<template>
  <svg :viewBox="`0 0 ${W} ${H}`" class="stat-bars" preserveAspectRatio="none">
    <line :x1="0" :y1="H - PAD_BOTTOM" :x2="W" :y2="H - PAD_BOTTOM" class="axis" />
    <g v-for="b in bars" :key="b.label">
      <rect :x="b.x" :y="b.y" :width="b.w" :height="b.h" rx="3" :fill="color ?? 'var(--md-primary)'">
        <title>{{ b.label }} · {{ fmt(b.value) }}</title>
      </rect>
      <text :x="b.x + b.w / 2" :y="H - 5" text-anchor="middle" class="tick">{{ b.label }}</text>
    </g>
  </svg>
</template>

<style scoped>
.stat-bars {
  width: 100%;
  height: 130px;
  display: block;
}
.axis {
  stroke: var(--md-outline-variant);
  stroke-width: 1;
}
.tick {
  fill: var(--md-on-surface-variant);
  font: 600 9px var(--font-body);
}
rect {
  opacity: 0.9;
}
rect:hover {
  opacity: 1;
}
</style>
