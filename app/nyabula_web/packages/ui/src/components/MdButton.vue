<script setup lang="ts">
/* `icon` renders a perfectly round, equal-width/height icon button;
 * other variants keep the original pill shape untouched. */
defineProps<{
  variant?: 'filled' | 'tonal' | 'outlined' | 'text' | 'icon';
  disabled?: boolean;
}>();
</script>

<template>
  <button class="md-btn" :class="variant ?? 'filled'" :disabled="disabled">
    <slot />
  </button>
</template>

<style scoped>
.md-btn {
  border: none;
  border-radius: var(--radius-full);
  padding: 10px 22px;
  font: 600 14px var(--font-body);
  cursor: pointer;
  transition: filter 0.15s, background 0.15s;
}
.md-btn:disabled {
  opacity: 0.4;
  cursor: default;
}
.md-btn:not(:disabled):hover {
  filter: brightness(1.1);
}
.filled {
  background: var(--md-primary);
  color: var(--md-on-primary);
}
.tonal {
  background: var(--md-secondary-container);
  color: var(--md-on-secondary-container);
}
.outlined {
  background: transparent;
  color: var(--md-primary);
  border: 1px solid var(--md-outline);
}
.text {
  background: transparent;
  color: var(--md-primary);
}
.icon {
  /* Perfect circle: fixed equal width/height, content centered. */
  width: 38px;
  height: 38px;
  padding: 0;
  border-radius: 50%;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  background: transparent;
  color: var(--md-on-surface-variant);
  font-size: 17px;
}
.icon:not(:disabled):hover {
  filter: none;
  color: var(--md-on-surface);
  background: var(--md-surface-container-high);
}
</style>
