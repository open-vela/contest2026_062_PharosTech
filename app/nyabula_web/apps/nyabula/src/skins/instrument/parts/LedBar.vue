<script setup lang="ts">
/* Segmented LED bar (horizontal or vertical). `value` is 0..1; segments
 * light in the signal color, or all warn/error tone when `tone` says so. */
import { computed } from 'vue';

const props = withDefaults(defineProps<{ value: number; segments?: number; vertical?: boolean; tone?: 'ok' | 'warn' | 'err' }>(), {
  segments: 20,
  vertical: false,
  tone: 'ok',
});
const lit = computed(() => Math.round(Math.max(0, Math.min(1, props.value)) * props.segments));
</script>

<template>
  <div class="led" :class="[{ v: vertical }, tone]" role="meter" :aria-valuenow="Math.round(value * 100)" aria-valuemin="0" aria-valuemax="100">
    <i v-for="i in segments" :key="i" :class="{ on: (vertical ? segments - i : i - 1) < lit }" />
  </div>
</template>

<style scoped>
.led { display: flex; gap: 2px; width: 100%; height: 10px; }
.led.v { flex-direction: column; width: 10px; height: 100%; }
i { flex: 1; background: var(--md-outline-variant); transition: background 0.14s steps(2, jump-none), box-shadow 0.14s; }
.ok i.on { background: var(--md-primary); box-shadow: 0 0 6px rgba(var(--md-primary-rgb), 0.45); }
.warn i.on { background: var(--md-warning); }
.err i.on { background: var(--md-error); }
</style>
