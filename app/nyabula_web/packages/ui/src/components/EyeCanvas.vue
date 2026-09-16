<script setup lang="ts">
/* Pure-render cat-eye canvas: hosts the EyeEngine with no store coupling.
 * - `eyeState` + `clockOffsetMs` props feed remote eye.state into the engine.
 * - Pointer drag emits `look` events (>=30ms throttle per protocol);
 *   pointer release emits `look` with {release:true}.
 * Host apps wire these to their own NyaLink transport. */
import { onMounted, onBeforeUnmount, ref, watch } from 'vue';
import { EyeEngine, type EyeState } from '@nyabula/eye-engine';

const props = defineProps<{
  eyeState?: EyeState | null;
  clockOffsetMs?: number;
  toyMode?: boolean;
}>();

const emit = defineEmits<{
  (e: 'ready', engine: EyeEngine): void;
  (e: 'look', data: { x?: number; y?: number; release?: boolean }): void;
}>();

const canvasRef = ref<HTMLCanvasElement | null>(null);
let engine: EyeEngine | null = null;
let lastLookSent = 0;
let pendingLook: { x: number; y: number } | null = null;
let lookTimer: ReturnType<typeof setTimeout> | null = null;

function cancelPendingLook() {
  if (lookTimer !== null) clearTimeout(lookTimer);
  lookTimer = null;
  pendingLook = null;
}

onMounted(() => {
  const cv = canvasRef.value!;
  engine = new EyeEngine(cv, {
    background: '#000',
    toyMode: props.toyMode,
    onInteraction: (x, y, phase) => {
      if (phase === 'up') {
        cancelPendingLook();
        emit('look', { release: true });
        return;
      }
      const now = performance.now();
      if (phase === 'move' && now - lastLookSent < 30) {
        pendingLook = { x, y };
        if (lookTimer === null) lookTimer = setTimeout(() => {
          const latest = pendingLook;
          cancelPendingLook();
          if (latest) {
            lastLookSent = performance.now();
            emit('look', latest);
          }
        }, 30 - (now - lastLookSent));
        return;
      }
      cancelPendingLook();
      lastLookSent = now;
      emit('look', { x, y });
    },
  });
  if (props.eyeState) engine.applyRemoteState(props.eyeState, props.clockOffsetMs ?? 0);
  engine.start();
  emit('ready', engine);
});

watch(() => props.toyMode, value => {
  if (!value) cancelPendingLook();
  engine?.setToyMode(value ?? false);
});

watch(
  () => props.eyeState,
  (s) => {
    if (s && engine) engine.applyRemoteState(s, props.clockOffsetMs ?? 0);
  },
);

onBeforeUnmount(() => {
  cancelPendingLook();
  engine?.destroy();
  engine = null;
});

defineExpose({ getEngine: () => engine });
</script>

<template>
  <canvas ref="canvasRef" class="eye-canvas" />
</template>

<style scoped>
.eye-canvas {
  width: 100%;
  height: 100%;
  display: block;
  border-radius: var(--radius-l);
  cursor: crosshair;
  touch-action: none;
}
</style>
