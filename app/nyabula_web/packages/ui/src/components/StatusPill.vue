<script setup lang="ts">
/* Prop-driven status pill: tone drives the dot color, label is free text.
 * Host apps compute both from their own connection stores. */
defineProps<{
  tone: 'ok' | 'busy' | 'off';
  label: string;
}>();
</script>

<template>
  <span class="pill" :class="tone" :title="label">
    <span class="dot" />
    <span class="label">{{ label }}</span>
  </span>
</template>

<style scoped>
.pill {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 6px 14px;
  border-radius: var(--radius-full);
  font: 600 12.5px var(--font-body);
  background: var(--md-surface-container-high);
  color: var(--md-on-surface-variant);
  max-width: 100%;
  min-width: 0;
}
/* Long device names stay on one line. */
.label { min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.dot {
  flex: none;
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--md-outline);
}
.pill.ok .dot {
  background: var(--md-success);
  box-shadow: 0 0 6px var(--md-success);
}
.pill.busy .dot {
  background: var(--md-warning);
  animation: blink 1s infinite alternate;
}
@keyframes blink {
  from {
    opacity: 0.35;
  }
  to {
    opacity: 1;
  }
}
</style>
