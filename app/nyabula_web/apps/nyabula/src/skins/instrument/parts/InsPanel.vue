<script setup lang="ts">
/* Instrument panel: flat module with hairline border, corner ticks and a
 * numbered monospace header ("01 / DEVICE 设备"). Header-right slot for
 * links or readouts. */
defineProps<{ idx?: string; code?: string; label?: string; flush?: boolean }>();
</script>

<template>
  <section class="ins-panel" :class="{ flush }">
    <header v-if="idx || code || label || $slots.aside" class="ph">
      <span class="ph-left">
        <span v-if="idx" class="ph-idx">{{ idx }}</span>
        <span v-if="code" class="ph-code">{{ code }}</span>
        <span v-if="label" class="ph-label">{{ label }}</span>
      </span>
      <span class="ph-aside"><slot name="aside" /></span>
    </header>
    <div class="pb"><slot /></div>
  </section>
</template>

<style scoped>
.ins-panel {
  position: relative;
  min-width: 0;
  display: flex;
  flex-direction: column;
  background: var(--md-surface-container);
  box-shadow: inset 0 0 0 1px var(--md-outline-variant);
  border-radius: 4px;
  animation: ins-panel-in 0.32s steps(4, jump-none) both;
}
.ins-panel::before,
.ins-panel::after {
  content: '';
  position: absolute;
  width: 7px;
  height: 7px;
  border: 0 solid var(--md-on-surface-variant);
  opacity: 0.7;
  pointer-events: none;
}
.ins-panel::before { top: -1px; left: -1px; border-width: 1px 0 0 1px; }
.ins-panel::after { bottom: -1px; right: -1px; border-width: 0 1px 1px 0; }
.ph {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  padding: 10px 14px;
  border-bottom: 1px solid var(--md-outline-variant);
  min-height: 38px;
}
.ph-left { display: flex; align-items: baseline; gap: 8px; min-width: 0; }
.ph-idx { font: 600 10.5px var(--ins-mono); color: var(--md-outline); letter-spacing: 0.1em; }
.ph-code { font: 700 10.5px var(--ins-mono); color: var(--md-on-surface); letter-spacing: 0.18em; }
.ph-label { font: 600 12px var(--font-body); color: var(--md-on-surface-variant); }
.ph-aside { display: flex; align-items: center; gap: 8px; min-width: 0; }
.pb { padding: 14px; display: flex; flex-direction: column; gap: 12px; flex: 1; min-height: 0; }
.flush .pb { padding: 0; }
@keyframes ins-panel-in { from { opacity: 0; } to { opacity: 1; } }
</style>
