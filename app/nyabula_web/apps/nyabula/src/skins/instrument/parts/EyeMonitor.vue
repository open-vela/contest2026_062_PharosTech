<script setup lang="ts">
/* Monitor bezel around the eye canvas: corner brackets, edge tick rulers,
 * a faint centre crosshair and four corner readout slots (tl/tr/bl/br).
 * Purely decorative; the default slot holds the real canvas / stage and
 * keeps all pointer interaction. */
defineProps<{ live?: boolean }>();
</script>

<template>
  <div class="monitor" :class="{ live }">
    <div class="screen"><slot /></div>
    <span class="br tl" /><span class="br tr" /><span class="br bl" /><span class="br brr" />
    <span class="ruler top" /><span class="ruler left" />
    <span class="cross" aria-hidden="true" />
    <span class="ro ro-tl"><slot name="tl" /></span>
    <span class="ro ro-tr"><slot name="tr" /></span>
    <span class="ro ro-bl"><slot name="bl" /></span>
    <span class="ro ro-br"><slot name="br" /></span>
  </div>
</template>

<style scoped>
.monitor {
  position: relative;
  width: 100%;
  height: 100%;
  min-height: 200px;
  padding: 14px;
  background: #000;
  border-radius: 4px;
  box-shadow: inset 0 0 0 1px var(--md-outline-variant);
  overflow: hidden;
  isolation: isolate;
}
.screen { position: relative; width: 100%; height: 100%; }
.screen :deep(.stage) { border-radius: 2px; box-shadow: none; background: transparent; min-height: 0; }
.br { position: absolute; width: 14px; height: 14px; border: 0 solid rgba(255, 255, 255, 0.55); pointer-events: none; z-index: 2; }
.live .br { border-color: var(--md-primary); }
.tl { top: 6px; left: 6px; border-width: 1px 0 0 1px; }
.tr { top: 6px; right: 6px; border-width: 1px 1px 0 0; }
.bl { bottom: 6px; left: 6px; border-width: 0 0 1px 1px; }
.brr { bottom: 6px; right: 6px; border-width: 0 1px 1px 0; }
.ruler { position: absolute; pointer-events: none; z-index: 2; opacity: 0.35; }
.ruler.top { top: 0; left: 30px; right: 30px; height: 5px; background: repeating-linear-gradient(90deg, #fff 0 1px, transparent 1px 12px); }
.ruler.left { left: 0; top: 30px; bottom: 30px; width: 5px; background: repeating-linear-gradient(180deg, #fff 0 1px, transparent 1px 12px); }
.cross {
  position: absolute;
  left: 50%;
  top: 50%;
  width: 26px;
  height: 26px;
  transform: translate(-50%, -50%);
  pointer-events: none;
  z-index: 2;
  opacity: 0.3;
  background:
    linear-gradient(#fff, #fff) center / 1px 100% no-repeat,
    linear-gradient(#fff, #fff) center / 100% 1px no-repeat;
}
.ro {
  position: absolute;
  z-index: 3;
  font: 600 10px/1.4 var(--ins-mono);
  letter-spacing: 0.12em;
  text-transform: uppercase;
  color: rgba(255, 255, 255, 0.72);
  pointer-events: none;
  white-space: nowrap;
}
.ro :deep(button), .ro :deep(a) { pointer-events: auto; }
.ro-tl { top: 12px; left: 26px; }
.ro-tr { top: 12px; right: 26px; text-align: right; }
.ro-bl { bottom: 12px; left: 26px; }
.ro-br { bottom: 12px; right: 26px; text-align: right; }
</style>
