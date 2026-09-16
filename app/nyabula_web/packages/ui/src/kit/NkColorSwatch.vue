<script setup lang="ts">
/* Colour picker: preset dots + a custom input[type=color]. modelValue is a
 * #rrggbb string. */
import { computed } from 'vue';
import UiIcon from '../components/UiIcon.vue';

export interface NkColorSwatchProps {
  modelValue: string;
  presets?: string[];
  label?: string;
  custom?: boolean;
  disabled?: boolean;
}
const props = withDefaults(defineProps<NkColorSwatchProps>(), {
  presets: () => ['#62dfaf', '#7ffcca', '#a4cddf', '#ffd94d', '#ffb4ab', '#ff8a65', '#b39ddb', '#ffffff'],
  custom: true,
  disabled: false,
});
const emit = defineEmits<{ (e: 'update:modelValue', v: string): void }>();

const norm = (c: string): string => (c || '').trim().toLowerCase();
const isPreset = computed(() => props.presets.some((p) => norm(p) === norm(props.modelValue)));
function pick(c: string): void {
  if (!props.disabled) emit('update:modelValue', c);
}
function onCustom(ev: Event): void {
  pick((ev.target as HTMLInputElement).value);
}
</script>

<template>
  <div class="nk-swatch" :class="{ disabled }" role="radiogroup" :aria-label="label">
    <div v-if="label" class="nk-swatch-label">{{ label }}</div>
    <div class="nk-swatch-list">
      <button
        v-for="c in presets"
        :key="c"
        type="button"
        class="nk-swatch-dot"
        :class="{ on: norm(c) === norm(modelValue) }"
        :style="{ '--c': c }"
        role="radio"
        :aria-checked="norm(c) === norm(modelValue)"
        :aria-label="c"
        @click="pick(c)"
      >
        <UiIcon v-if="norm(c) === norm(modelValue)" name="check" :size="18" />
      </button>
      <label v-if="custom" class="nk-swatch-dot custom" :class="{ on: !isPreset }" :style="{ '--c': modelValue }" aria-label="自定义颜色">
        <input type="color" :value="modelValue" :disabled="disabled" @input="onCustom" />
        <UiIcon name="palette" :size="18" />
      </label>
    </div>
  </div>
</template>

<style scoped>
.nk-swatch.disabled { opacity: 0.45; pointer-events: none; }
.nk-swatch-label { font: 600 12px var(--font-body); color: var(--md-on-surface-variant); margin-bottom: 8px; }
.nk-swatch-list { display: flex; flex-wrap: wrap; gap: 10px; }
.nk-swatch-dot {
  position: relative;
  width: 44px; height: 44px; padding: 0;
  border: 3px solid var(--md-surface-container-highest);
  border-radius: var(--radius-full);
  background: var(--c);
  color: #000;
  display: grid; place-items: center;
  cursor: pointer;
  transition: border-color var(--dur-fast), transform var(--dur-fast);
}
.nk-swatch-dot:active { transform: scale(0.94); }
.nk-swatch-dot.on { border-color: var(--md-primary); }
.nk-swatch-dot:focus-visible { outline: 2px solid var(--md-primary); outline-offset: 2px; }
.nk-swatch-dot.custom { color: var(--md-on-surface); background: var(--md-surface-container-high); }
.nk-swatch-dot.custom.on { background: var(--c); }
.nk-swatch-dot.custom input { position: absolute; inset: 0; opacity: 0; width: 100%; height: 100%; cursor: pointer; }
.nk-swatch-dot .ui-icon { mix-blend-mode: difference; color: #fff; }
</style>
