<script setup lang="ts">
defineProps<{ modelValue: boolean; disabled?: boolean; label?: string }>();
const emit = defineEmits<{ (e: 'update:modelValue', v: boolean): void }>();
</script>

<template>
  <label class="md-switch" :class="{ on: modelValue, disabled }">
    <span v-if="label" class="sw-label">{{ label }}</span>
    <span class="track" role="switch" :aria-checked="modelValue" tabindex="0"
      @click="!disabled && emit('update:modelValue', !modelValue)"
      @keydown.space.prevent="!disabled && emit('update:modelValue', !modelValue)">
      <span class="thumb" />
    </span>
  </label>
</template>

<style scoped>
.md-switch { display: inline-flex; align-items: center; gap: 12px; cursor: pointer; user-select: none; }
.md-switch.disabled { opacity: 0.45; cursor: default; }
.sw-label { font-size: 14px; color: var(--md-on-surface); }
.track {
  position: relative;
  width: 48px;
  height: 28px;
  border-radius: 999px;
  background: var(--md-surface-container-highest);
  border: 2px solid var(--md-outline);
  transition: background var(--dur-fast), border-color var(--dur-fast);
  flex: none;
}
.thumb {
  position: absolute;
  top: 4px;
  left: 4px;
  width: 16px;
  height: 16px;
  border-radius: 50%;
  background: var(--md-outline);
  transition: transform var(--dur-fast) var(--ease-spring), background var(--dur-fast), width var(--dur-fast), height var(--dur-fast), top var(--dur-fast);
}
.on .track { background: var(--md-primary); border-color: var(--md-primary); }
.on .thumb { transform: translateX(20px); background: var(--md-on-primary); width: 20px; height: 20px; top: 2px; }
.track:focus-visible { outline: 2px solid var(--md-primary); outline-offset: 2px; }
</style>
