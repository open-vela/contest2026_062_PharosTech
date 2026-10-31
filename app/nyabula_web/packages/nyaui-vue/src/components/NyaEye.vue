<script setup lang="ts">
/* Embedded static cat-eye preview: a small EyeEngine canvas pinned to a fixed
 * expression mode. Started on mount, stopped/destroyed on unmount. */
import { onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { EyeEngine } from '@nyabula/eye-engine';

const props = defineProps<{ mode?: string; height?: number }>();

const canvasRef = ref<HTMLCanvasElement | null>(null);
let engine: EyeEngine | null = null;

onMounted(() => {
  const cv = canvasRef.value;
  if (!cv) return;
  try {
    engine = new EyeEngine(cv);
    engine.setMode(props.mode ?? 'idle');
    engine.start();
  } catch {
    // Canvas2D unavailable (tests / headless) — leave the canvas blank.
    engine = null;
  }
});

watch(
  () => props.mode,
  (m) => engine?.setMode(m ?? 'idle'),
);

onBeforeUnmount(() => {
  engine?.stop();
  engine?.destroy();
  engine = null;
});
</script>

<template>
  <canvas ref="canvasRef" class="nya-eye" :style="{ height: (height ?? 120) + 'px' }" />
</template>

<style scoped>
.nya-eye {
  width: 100%;
  display: block;
  border-radius: var(--radius-m, 14px);
  background: #000;
}
</style>
