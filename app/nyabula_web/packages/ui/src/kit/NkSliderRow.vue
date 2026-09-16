<script setup lang="ts">
/* Title + live value + MdSlider with optional leading/trailing icons.
 * `update:modelValue` fires while dragging; `commit` fires on release. */
import MdSlider from '../components/MdSlider.vue';
import UiIcon from '../components/UiIcon.vue';

export interface NkSliderRowProps {
  modelValue: number;
  title: string;
  min?: number;
  max?: number;
  step?: number;
  unit?: string;
  iconStart?: string;
  iconEnd?: string;
  disabled?: boolean;
}
const props = withDefaults(defineProps<NkSliderRowProps>(), { min: 0, max: 100, step: 1, unit: '', disabled: false });
const emit = defineEmits<{ (e: 'update:modelValue', v: number): void; (e: 'commit', v: number): void }>();

function onInput(v: number): void {
  if (!props.disabled) emit('update:modelValue', v);
}
function onChange(ev: Event): void {
  if (!props.disabled) emit('commit', Number((ev.target as HTMLInputElement).value));
}
</script>

<template>
  <div class="nk-slider-row" :class="{ disabled }">
    <div class="nk-slider-head">
      <span class="nk-slider-title">{{ title }}</span>
      <span class="nk-slider-val">{{ modelValue }}<span v-if="unit" class="nk-slider-unit">{{ unit }}</span></span>
    </div>
    <div class="nk-slider-track">
      <UiIcon v-if="iconStart" :name="iconStart" :size="20" class="nk-slider-ico" />
      <MdSlider
        :model-value="modelValue"
        :min="min"
        :max="max"
        :step="step"
        :disabled="disabled"
        @update:model-value="onInput"
        @change="onChange"
      />
      <UiIcon v-if="iconEnd" :name="iconEnd" :size="20" class="nk-slider-ico" />
    </div>
  </div>
</template>

<style scoped>
.nk-slider-row { padding: 10px 12px; color: var(--md-on-surface); }
.nk-slider-row.disabled { opacity: 0.45; pointer-events: none; }
.nk-slider-head { display: flex; justify-content: space-between; align-items: baseline; margin-bottom: 6px; }
.nk-slider-title { font: 600 14px var(--font-body); }
.nk-slider-val { font: 700 16px var(--font-title); color: var(--md-primary); }
.nk-slider-unit { font: 600 12px var(--font-body); margin-left: 2px; }
.nk-slider-track { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.nk-slider-track :deep(.md-slider) { flex: 1; }
.nk-slider-ico { color: var(--md-on-surface-variant); }
</style>
