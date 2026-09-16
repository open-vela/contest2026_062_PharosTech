<script setup lang="ts">
defineProps<{ selected?: boolean }>();
</script>

<template>
  <button class="md-chip" :class="{ selected }">
    <slot />
  </button>
</template>

<style scoped>
.md-chip {
  border: 1px solid var(--md-outline-variant);
  background: transparent;
  color: var(--md-on-surface-variant);
  border-radius: var(--radius-s);
  padding: 7px 14px;
  font: 600 13px var(--font-body);
  cursor: pointer;
  transition: background 0.15s, color 0.15s, border-color 0.15s;
}
.md-chip:hover {
  border-color: var(--md-outline);
  color: var(--md-on-surface);
}
.md-chip.selected {
  background: var(--md-secondary-container);
  color: var(--md-on-secondary-container);
  border-color: transparent;
}
</style>
