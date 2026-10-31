<script setup lang="ts">
/* Thin progress ring with a glowing arc that draws in on mount. `value`
 * null renders an empty track (unknown reading). */
import { computed } from 'vue';

const props = withDefaults(defineProps<{ value: number | null; size?: number; stroke?: number; color?: string }>(), {
  size: 50,
  stroke: 4,
  color: 'var(--nb-acc)',
});
const r = computed(() => (props.size - props.stroke) / 2);
const c = computed(() => 2 * Math.PI * r.value);
const off = computed(() => c.value * (1 - Math.max(0, Math.min(1, props.value ?? 0))));
</script>

<template>
  <span class="nb-ring" :style="{ width: size + 'px', height: size + 'px' }">
    <svg :width="size" :height="size" :viewBox="`0 0 ${size} ${size}`" aria-hidden="true">
      <circle :cx="size / 2" :cy="size / 2" :r="r" fill="none" class="track" :stroke-width="stroke" />
      <circle
        v-if="value !== null"
        class="arc"
        :cx="size / 2"
        :cy="size / 2"
        :r="r"
        fill="none"
        :stroke-width="stroke"
        stroke-linecap="round"
        :stroke-dasharray="c"
        :stroke-dashoffset="off"
        :style="{ '--c': c, stroke: color, filter: `drop-shadow(0 0 4px ${color})` }"
        :transform="`rotate(-90 ${size / 2} ${size / 2})`"
      />
    </svg>
    <span class="slot"><slot /></span>
  </span>
</template>

<style scoped>
.nb-ring { position: relative; display: inline-grid; place-items: center; flex: none; }
svg { position: absolute; inset: 0; }
.track { stroke: var(--nb-soft-2); }
.arc { transition: stroke-dashoffset 1s cubic-bezier(0.22, 1, 0.36, 1); animation: nb-draw 1.4s cubic-bezier(0.22, 1, 0.36, 1) both; }
@keyframes nb-draw { from { stroke-dashoffset: var(--c); } }
.slot { position: relative; display: grid; place-items: center; color: var(--nb-sub); }
</style>
