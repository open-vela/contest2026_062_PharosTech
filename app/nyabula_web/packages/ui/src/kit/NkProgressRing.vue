<script setup lang="ts">
/* Full-circle progress ring (0-1) with a default slot rendered in the middle. */
import { computed } from 'vue';

export interface NkProgressRingProps {
  /** 0..1 */
  value: number;
  size?: number;
  stroke?: number;
  indeterminate?: boolean;
  tone?: 'primary' | 'ok' | 'warn' | 'error';
}
const props = withDefaults(defineProps<NkProgressRingProps>(), { size: 96, stroke: 8, indeterminate: false, tone: 'primary' });
const r = computed(() => (props.size - props.stroke) / 2);
const circ = computed(() => 2 * Math.PI * r.value);
const frac = computed(() => Math.min(1, Math.max(0, Number.isFinite(props.value) ? props.value : 0)));
const offset = computed(() => circ.value * (1 - frac.value));
</script>

<template>
  <div class="nk-ring" :class="['tone-' + tone, { indeterminate }]" :style="{ width: size + 'px', height: size + 'px' }"
    role="progressbar" :aria-valuenow="indeterminate ? undefined : Math.round(frac * 100)" aria-valuemin="0" aria-valuemax="100">
    <svg :viewBox="`0 0 ${size} ${size}`" :width="size" :height="size">
      <circle class="nk-ring-track" :cx="size / 2" :cy="size / 2" :r="r" :stroke-width="stroke" />
      <circle class="nk-ring-fill" :cx="size / 2" :cy="size / 2" :r="r" :stroke-width="stroke"
        :stroke-dasharray="indeterminate ? `${circ * 0.25} ${circ}` : circ"
        :stroke-dashoffset="indeterminate ? 0 : offset"
        :transform="`rotate(-90 ${size / 2} ${size / 2})`" />
    </svg>
    <div class="nk-ring-center"><slot>{{ Math.round(frac * 100) }}%</slot></div>
  </div>
</template>

<style scoped>
.nk-ring { position: relative; display: inline-grid; place-items: center; color: var(--md-on-surface); }
.nk-ring svg { position: absolute; inset: 0; }
.nk-ring.indeterminate svg { animation: nk-ring-spin 1.2s linear infinite; }
@keyframes nk-ring-spin { to { transform: rotate(360deg); } }
.nk-ring-track { fill: none; stroke: var(--md-surface-container-highest); }
.nk-ring-fill { fill: none; stroke: var(--md-primary); stroke-linecap: round; transition: stroke-dashoffset var(--dur) var(--ease-out); }
.tone-ok .nk-ring-fill { stroke: var(--md-success); }
.tone-warn .nk-ring-fill { stroke: var(--md-warning); }
.tone-error .nk-ring-fill { stroke: var(--md-error); }
.nk-ring-center { position: relative; font: 700 16px var(--font-title); text-align: center; }
</style>
