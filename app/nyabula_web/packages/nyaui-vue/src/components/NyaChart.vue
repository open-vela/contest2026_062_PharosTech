<script setup lang="ts">
/* Inline-SVG line/bar chart, no chart library (same approach as console's
 * StatBars). Points are capped at 120 per the DSL contract. */
import { computed } from 'vue';

const props = defineProps<{
  kind?: 'line' | 'bar';
  points?: number[];
  labels?: string[];
  height?: number;
}>();

const W = 320;
const PAD_BOTTOM = 16;

const h = computed(() => props.height ?? 120);
const pts = computed(() => (props.points ?? []).slice(0, 120));
const max = computed(() => Math.max(1, ...pts.value));
const plotH = computed(() => h.value - PAD_BOTTOM - 6);

const bars = computed(() => {
  const n = pts.value.length || 1;
  const slot = W / n;
  const bw = Math.min(26, slot * 0.6);
  return pts.value.map((v, i) => {
    const bh = (v / max.value) * plotH.value;
    return {
      x: i * slot + (slot - bw) / 2,
      y: h.value - PAD_BOTTOM - bh,
      w: bw,
      h: Math.max(v > 0 ? 2 : 0, bh),
      cx: i * slot + slot / 2,
      v,
    };
  });
});

const linePath = computed(() => {
  const n = pts.value.length;
  if (n === 0) return '';
  const slot = W / Math.max(1, n - 1 || 1);
  return pts.value
    .map((v, i) => {
      const x = n === 1 ? W / 2 : i * slot;
      const y = h.value - PAD_BOTTOM - (v / max.value) * plotH.value;
      return `${i === 0 ? 'M' : 'L'}${x.toFixed(1)},${y.toFixed(1)}`;
    })
    .join(' ');
});
</script>

<template>
  <svg :viewBox="`0 0 ${W} ${h}`" class="nya-chart" :style="{ height: h + 10 + 'px' }" preserveAspectRatio="none">
    <line :x1="0" :y1="h - PAD_BOTTOM" :x2="W" :y2="h - PAD_BOTTOM" class="axis" />
    <template v-if="(kind ?? 'line') === 'bar'">
      <g v-for="(b, i) in bars" :key="i">
        <rect :x="b.x" :y="b.y" :width="b.w" :height="b.h" rx="3" class="bar">
          <title>{{ labels?.[i] ?? i }} · {{ b.v }}</title>
        </rect>
      </g>
    </template>
    <path v-else :d="linePath" class="line" fill="none" />
    <g v-if="labels?.length">
      <text
        v-for="(b, i) in bars"
        :key="'l' + i"
        :x="b.cx"
        :y="h - 4"
        text-anchor="middle"
        class="tick"
      >
        {{ labels[i] ?? '' }}
      </text>
    </g>
  </svg>
</template>

<style scoped>
.nya-chart {
  width: 100%;
  display: block;
}
.axis {
  stroke: var(--md-outline-variant);
  stroke-width: 1;
}
.bar {
  fill: var(--md-primary);
  opacity: 0.9;
}
.bar:hover {
  opacity: 1;
}
.line {
  stroke: var(--md-primary);
  stroke-width: 2.5;
  stroke-linejoin: round;
  stroke-linecap: round;
}
.tick {
  fill: var(--md-on-surface-variant);
  font: 600 9px var(--font-body, sans-serif);
}
</style>
