<script setup lang="ts">
/* Segmented LED bar. Lit segments glow the primary (signal) color; when
 * the value is at or below `warnBelow` they glow the error color. */
import { computed } from 'vue';

const props = withDefaults(defineProps<{ value: number; segments?: number; warnBelow?: number }>(), {
  segments: 14,
  warnBelow: -1,
});
const lit = computed(() => Math.round(Math.max(0, Math.min(1, props.value)) * props.segments));
const warn = computed(() => props.value <= props.warnBelow);
</script>

<template>
  <div class="led" :class="{ warn }" role="meter" :aria-valuenow="Math.round(value * 100)" aria-valuemin="0" aria-valuemax="100">
    <i v-for="i in segments" :key="i" :class="{ on: i <= lit }" />
  </div>
</template>

<style scoped>
.led { display: flex; gap: 2px; width: 100%; height: 10px; }
i { flex: 1; background: var(--md-outline-variant); border-radius: 1px; transition: background 140ms steps(2, jump-none), box-shadow 140ms; }
i.on { background: var(--md-primary); box-shadow: 0 0 6px rgba(var(--md-primary-rgb), 0.45); }
.warn i.on { background: var(--md-error); box-shadow: none; }
</style>
