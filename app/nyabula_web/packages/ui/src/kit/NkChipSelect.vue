<script setup lang="ts">
/* Single/multi select chips. Single: modelValue is string; multi: string[]. */
import UiIcon from '../components/UiIcon.vue';

export interface NkChipOption {
  id: string;
  label: string;
  icon?: string;
}
export interface NkChipSelectProps {
  modelValue: string | string[] | undefined;
  options: NkChipOption[];
  multi?: boolean;
  label?: string;
  disabled?: boolean;
}
const props = withDefaults(defineProps<NkChipSelectProps>(), { multi: false, disabled: false });
const emit = defineEmits<{ (e: 'update:modelValue', v: string | string[]): void }>();

function selected(id: string): boolean {
  return props.multi ? Array.isArray(props.modelValue) && props.modelValue.includes(id) : props.modelValue === id;
}
function toggle(id: string): void {
  if (props.disabled) return;
  if (props.multi) {
    const cur = Array.isArray(props.modelValue) ? props.modelValue : [];
    emit('update:modelValue', cur.includes(id) ? cur.filter((c) => c !== id) : [...cur, id]);
  } else {
    emit('update:modelValue', id);
  }
}
</script>

<template>
  <div class="nk-chips" :class="{ disabled }" :role="multi ? 'group' : 'radiogroup'">
    <div v-if="label" class="nk-chips-label">{{ label }}</div>
    <div class="nk-chips-list">
      <button
        v-for="o in options"
        :key="o.id"
        type="button"
        class="nk-chip"
        :class="{ selected: selected(o.id) }"
        :role="multi ? 'checkbox' : 'radio'"
        :aria-checked="selected(o.id)"
        @click="toggle(o.id)"
      >
        <UiIcon v-if="o.icon" :name="o.icon" :size="18" />
        <span>{{ o.label }}</span>
      </button>
    </div>
  </div>
</template>

<style scoped>
.nk-chips.disabled { opacity: 0.45; pointer-events: none; }
.nk-chips-label { font: 600 12px var(--font-body); color: var(--md-on-surface-variant); margin-bottom: 8px; }
.nk-chips-list { display: flex; flex-wrap: wrap; gap: 8px; }
.nk-chip {
  display: inline-flex; align-items: center; gap: 6px;
  min-height: 40px; padding: 8px 14px;
  border: 1px solid var(--md-outline-variant);
  border-radius: var(--radius-full);
  background: transparent;
  color: var(--md-on-surface-variant);
  font: 600 13px var(--font-body);
  cursor: pointer;
  transition: background var(--dur-fast), color var(--dur-fast), border-color var(--dur-fast);
}
.nk-chip.selected { background: var(--md-secondary-container); color: var(--md-on-secondary-container); border-color: transparent; }
.nk-chip:focus-visible { outline: 2px solid var(--md-primary); outline-offset: 2px; }
</style>
