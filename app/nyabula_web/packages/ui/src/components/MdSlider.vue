<script setup lang="ts">
const props = defineProps<{
  modelValue: number;
  min?: number;
  max?: number;
  step?: number;
}>();
const emit = defineEmits<{ (e: 'update:modelValue', v: number): void }>();

function onInput(ev: Event) {
  emit('update:modelValue', Number((ev.target as HTMLInputElement).value));
}
</script>

<template>
  <input
    class="md-slider"
    type="range"
    :min="props.min ?? 0"
    :max="props.max ?? 100"
    :step="props.step ?? 1"
    :value="props.modelValue"
    @input="onInput"
  />
</template>

<style scoped>
.md-slider {
  -webkit-appearance: none;
  appearance: none;
  width: 100%;
  height: 4px;
  border-radius: 2px;
  background: var(--md-surface-container-highest);
  outline: none;
  accent-color: var(--md-primary);
}
.md-slider::-webkit-slider-thumb {
  -webkit-appearance: none;
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: var(--md-primary);
  cursor: pointer;
  border: none;
  box-shadow: var(--md-elev-1);
}
.md-slider::-moz-range-thumb {
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: var(--md-primary);
  cursor: pointer;
  border: none;
}
</style>
