<script setup lang="ts">
import { ref } from 'vue';
import UiIcon from './UiIcon.vue';

defineProps<{
  modelValue: string;
  label?: string;
  placeholder?: string;
  type?: string;
  icon?: string;
  disabled?: boolean;
  error?: string | null;
  hint?: string;
  inputmode?: 'text' | 'numeric' | 'decimal' | 'url' | 'email' | 'tel' | 'search';
  autocomplete?: string;
}>();
const emit = defineEmits<{ (e: 'update:modelValue', v: string): void; (e: 'enter'): void }>();
const el = ref<HTMLInputElement | null>(null);
defineExpose({ focus: () => el.value?.focus() });
</script>

<template>
  <label class="tf" :class="{ err: !!error, disabled }">
    <span v-if="label" class="tf-label">{{ label }}</span>
    <span class="tf-box">
      <UiIcon v-if="icon" :name="icon" :size="18" class="tf-icon" />
      <input
        ref="el"
        :value="modelValue"
        :type="type ?? 'text'"
        :placeholder="placeholder"
        :disabled="disabled"
        :inputmode="inputmode"
        :autocomplete="autocomplete"
        spellcheck="false"
        @input="emit('update:modelValue', ($event.target as HTMLInputElement).value)"
        @keyup.enter="emit('enter')"
      />
    </span>
    <span v-if="error" class="tf-msg err">{{ error }}</span>
    <span v-else-if="hint" class="tf-msg">{{ hint }}</span>
  </label>
</template>

<style scoped>
.tf { display: flex; flex-direction: column; gap: 6px; width: 100%; }
.tf.disabled { opacity: 0.5; }
.tf-label { font-size: 12.5px; color: var(--md-on-surface-variant); letter-spacing: 0.02em; }
.tf-box {
  display: flex;
  align-items: center;
  gap: 8px;
  background: var(--md-surface-container-high);
  border: 1px solid var(--md-outline-variant);
  border-radius: var(--radius-m);
  padding: 0 12px;
  transition: border-color var(--dur-fast), box-shadow var(--dur-fast);
}
.tf-box:focus-within { border-color: var(--md-primary); box-shadow: 0 0 0 3px rgba(var(--md-primary-rgb), 0.18); }
.tf.err .tf-box { border-color: var(--md-error); }
.tf-icon { color: var(--md-on-surface-variant); }
input {
  flex: 1;
  min-width: 0;
  background: transparent;
  border: none;
  outline: none;
  color: var(--md-on-surface);
  font: 500 14.5px var(--font-body);
  padding: 11px 0;
}
input::placeholder { color: var(--md-outline); }
.tf-msg { font-size: 12px; color: var(--md-on-surface-variant); }
.tf-msg.err { color: var(--md-error); }
</style>
