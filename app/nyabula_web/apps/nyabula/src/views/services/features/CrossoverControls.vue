<script setup lang="ts">
import { computed } from 'vue';

const props = defineProps<{
  satelliteHighpassHz: number;
  bassLowpassHz: number;
  disabled?: boolean;
}>();
const emit = defineEmits<{
  'update:satelliteHighpassHz': [value: number];
  'update:bassLowpassHz': [value: number];
}>();
const coverage = computed(() => {
  const high = props.satelliteHighpassHz, low = props.bassLowpassHz;
  if (high < low) return `重叠频段 · ${high}–${low} Hz`;
  if (high === low) return `共同分频点 · ${high} Hz`;
  return `两路截止点之间有间隔 · ${low}–${high} Hz`;
});
</script>

<template>
  <div class="crossover-controls" aria-label="独立分频设置">
    <label class="crossover-range">
      <span>板载高通 · 保留中高频</span><output>{{satelliteHighpassHz}} Hz</output>
      <input type="range" min="40" max="500" step="5" :value="satelliteHighpassHz" :disabled="disabled"
        aria-label="板载高通 Hz" @input="emit('update:satelliteHighpassHz', Number(($event.target as HTMLInputElement).value))"/>
    </label>
    <label class="crossover-range">
      <span>低音低通 · 保留低频与中低频</span><output>{{bassLowpassHz}} Hz</output>
      <input type="range" min="40" max="500" step="5" :value="bassLowpassHz" :disabled="disabled"
        aria-label="低音低通 Hz" @input="emit('update:bassLowpassHz', Number(($event.target as HTMLInputElement).value))"/>
    </label>
    <p class="coverage" role="status">{{coverage}}<small>Linkwitz–Riley · 24 dB/oct，交界平滑衰减</small></p>
  </div>
</template>

<style scoped>
.crossover-controls{display:grid;gap:20px}
.crossover-range{display:grid;grid-template-columns:1fr auto;gap:12px;font-size:14px}
.crossover-range output{color:var(--md-primary);font-variant-numeric:tabular-nums}
.crossover-range input{grid-column:1/-1;width:100%;margin:0;min-height:24px;accent-color:var(--md-primary)}
.coverage{margin:0;font-size:14px;color:var(--md-on-surface)}
.coverage small{display:block;margin-top:6px;font-size:12px;color:var(--md-on-surface-variant)}
</style>
