<script setup lang="ts">
/* Ring gauge (0-100). Colour follows thresholds: value >= warnAt -> warning,
 * >= errorAt -> error, else primary. `invert` flips the comparison so low
 * values (battery, food level) turn warning/error. */
import { computed } from 'vue';

export interface NkGaugeProps {
  value: number;
  label?: string;
  unit?: string;
  size?: number;
  stroke?: number;
  warnAt?: number;
  errorAt?: number;
  invert?: boolean;
}
const props = withDefaults(defineProps<NkGaugeProps>(), { size: 120, stroke: 10, unit: '%', invert: false });

const clamped = computed(() => Math.min(100, Math.max(0, Number.isFinite(props.value) ? props.value : 0)));
const r = computed(() => (props.size - props.stroke) / 2);
/* 270 degree arc: start at 135deg, sweep clockwise. */
const circ = computed(() => 2 * Math.PI * r.value);
const arcLen = computed(() => circ.value * 0.75);
const dash = computed(() => `${(clamped.value / 100) * arcLen.value} ${circ.value}`);
const tone = computed(() => {
  const v = clamped.value;
  const hit = (t: number | undefined) => t !== undefined && (props.invert ? v <= t : v >= t);
  if (hit(props.errorAt)) return 'error';
  if (hit(props.warnAt)) return 'warn';
  return 'ok';
});
</script>

<template>
  <div class="nk-gauge" :class="'tone-' + tone" :style="{ width: size + 'px', height: size + 'px' }" role="meter" :aria-valuenow="clamped" aria-valuemin="0" aria-valuemax="100">
    <svg :viewBox="`0 0 ${size} ${size}`" :width="size" :height="size">
      <circle class="nk-gauge-track" :cx="size / 2" :cy="size / 2" :r="r" :stroke-width="stroke"
        :stroke-dasharray="`${arcLen} ${circ}`" :transform="`rotate(135 ${size / 2} ${size / 2})`" />
      <circle class="nk-gauge-fill" :cx="size / 2" :cy="size / 2" :r="r" :stroke-width="stroke"
        :stroke-dasharray="dash" :transform="`rotate(135 ${size / 2} ${size / 2})`" />
    </svg>
    <div class="nk-gauge-center">
      <span class="nk-gauge-value" :style="{ fontSize: size * 0.24 + 'px' }">{{ Math.round(clamped) }}<span class="nk-gauge-unit">{{ unit }}</span></span>
      <span v-if="label" class="nk-gauge-label" :style="{ fontSize: Math.max(11, size * 0.1) + 'px' }">{{ label }}</span>
    </div>
  </div>
</template>

<style scoped>
.nk-gauge { position: relative; display: inline-grid; place-items: center; color: var(--md-on-surface); }
.nk-gauge svg { position: absolute; inset: 0; }
.nk-gauge-track { fill: none; stroke: var(--md-surface-container-highest); stroke-linecap: round; }
.nk-gauge-fill { fill: none; stroke: var(--md-primary); stroke-linecap: round; transition: stroke-dasharray var(--dur) var(--ease-out), stroke var(--dur-fast); }
.tone-warn .nk-gauge-fill { stroke: var(--md-warning); }
.tone-error .nk-gauge-fill { stroke: var(--md-error); }
.nk-gauge-center { position: relative; display: flex; flex-direction: column; align-items: center; gap: 2px; }
.nk-gauge-value { font: 700 1em var(--font-title); line-height: 1; }
.nk-gauge-unit { font: 600 0.5em var(--font-body); color: var(--md-on-surface-variant); margin-left: 2px; }
.nk-gauge-label { font: 600 1em var(--font-body); color: var(--md-on-surface-variant); }
</style>
