<script setup lang="ts">
/* Dependency-free inline SVG bar chart of onlineSeconds per day. Bars use
 * --md-primary; the hovered bar shows an inline label. */
import { computed, ref } from 'vue';
import type { DailyStat } from '../../api/cloud';
import { formatDuration } from './accountDevice.logic';

const props = defineProps<{ daily: DailyStat[]; height?: number }>();

const W = 640;
const H = computed(() => props.height ?? 220);
const PAD = { l: 44, r: 12, t: 16, b: 30 };
const hover = ref<number | null>(null);

const max = computed(() => Math.max(3600, ...props.daily.map((d) => d.onlineSeconds)));
const innerW = computed(() => W - PAD.l - PAD.r);
const innerH = computed(() => H.value - PAD.t - PAD.b);
const step = computed(() => (props.daily.length ? innerW.value / props.daily.length : innerW.value));
const barW = computed(() => Math.max(4, Math.min(36, step.value * 0.6)));

const bars = computed(() =>
  props.daily.map((d, i) => {
    const h = (d.onlineSeconds / max.value) * innerH.value;
    return {
      i,
      x: PAD.l + step.value * i + (step.value - barW.value) / 2,
      y: PAD.t + innerH.value - h,
      h,
      cx: PAD.l + step.value * i + step.value / 2,
      label: d.date.slice(5),
      value: d.onlineSeconds,
    };
  }),
);
/* Y axis: 4 ticks in hours. */
const ticks = computed(() =>
  [0, 0.25, 0.5, 0.75, 1].map((f) => ({
    y: PAD.t + innerH.value - f * innerH.value,
    label: `${Math.round((f * max.value) / 360) / 10}h`,
  })),
);
/* Thin x labels when there are many days. */
const labelEvery = computed(() => Math.max(1, Math.ceil(props.daily.length / 10)));
</script>

<template>
  <svg class="chart" :viewBox="`0 0 ${W} ${H}`" preserveAspectRatio="none" role="img" aria-label="每日在线时长">
    <g class="grid">
      <line v-for="t in ticks" :key="t.y" :x1="PAD.l" :x2="W - PAD.r" :y1="t.y" :y2="t.y" />
      <text v-for="t in ticks" :key="'l' + t.y" :x="PAD.l - 6" :y="t.y + 4" text-anchor="end">{{ t.label }}</text>
    </g>
    <g v-for="b in bars" :key="b.i" @mouseenter="hover = b.i" @mouseleave="hover = null" @click="hover = hover === b.i ? null : b.i">
      <rect :x="PAD.l + step * b.i" :y="PAD.t" :width="step" :height="innerH" fill="transparent" />
      <rect class="bar" :class="{ on: hover === b.i }" :x="b.x" :y="b.y" :width="barW" :height="Math.max(b.h, b.value ? 2 : 0)" rx="3" />
      <text v-if="b.i % labelEvery === 0" class="xl" :x="b.cx" :y="H - 10" text-anchor="middle">{{ b.label }}</text>
    </g>
    <g v-if="hover !== null && bars[hover]" class="tip">
      <text :x="bars[hover].cx" :y="Math.max(PAD.t + 10, bars[hover].y - 6)" text-anchor="middle">{{ formatDuration(bars[hover].value) }}</text>
    </g>
    <text v-if="!bars.length" :x="W / 2" :y="H / 2" text-anchor="middle" class="empty">暂无数据</text>
  </svg>
</template>

<style scoped>
.chart { width: 100%; height: auto; display: block; font-family: var(--font-body); overflow: visible; }
.grid line { stroke: var(--md-outline-variant); stroke-width: 1; }
.grid text, .xl, .empty { fill: var(--md-on-surface-variant); font-size: 11px; }
.bar { fill: var(--md-primary); opacity: 0.75; transition: opacity var(--dur-fast); cursor: pointer; }
.bar.on { opacity: 1; }
.tip text { fill: var(--md-on-surface); font-size: 12px; font-weight: 600; pointer-events: none; }
</style>
