<script setup lang="ts">
/* EyeEngine host for the Nebula skin: same contract as @nyabula/ui's
 * EyeCanvas (remote eye.state in, throttled `look` out, `ready` with the
 * engine) but with a transparent background so the eyes float on the
 * visor, plus an optional standalone `mode` for pages without a device
 * (connect). `interactive=false` detaches pointer handling entirely. */
import { onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { EyeEngine, type EyeState } from '@nyabula/eye-engine';

const props = withDefaults(
  defineProps<{
    eyeState?: EyeState | null;
    clockOffsetMs?: number;
    toyMode?: boolean;
    interactive?: boolean;
    /** Standalone expression when no remote state drives the engine. */
    mode?: string;
  }>(),
  { interactive: true },
);
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
  engine = new EyeEngine(canvasRef.value!, {
    background: 'transparent',
    toyMode: props.toyMode,
    attachPointer: props.interactive,
    onInteraction: (x, y, phase) => {
      if (phase === 'up') {
        cancelPendingLook();
        emit('look', { release: true });
        return;
      }
      // Protocol: at most one look every 30ms, trailing sample kept.
      const now = performance.now();
      if (phase === 'move' && now - lastLookSent < 30) {
        pendingLook = { x, y };
        if (lookTimer === null) {
          lookTimer = setTimeout(() => {
            const latest = pendingLook;
            cancelPendingLook();
            if (latest) {
              lastLookSent = performance.now();
              emit('look', latest);
            }
          }, 30 - (now - lastLookSent));
        }
        return;
      }
      cancelPendingLook();
      lastLookSent = now;
      emit('look', { x, y });
    },
  });
  if (props.eyeState) engine.applyRemoteState(props.eyeState, props.clockOffsetMs ?? 0);
  else if (props.mode) engine.setMode(props.mode);
  engine.start();
  emit('ready', engine);
});

watch(
  () => props.toyMode,
  (v) => {
    if (!v) cancelPendingLook();
    engine?.setToyMode(v ?? false);
  },
);
watch(
  () => props.eyeState,
  (s) => {
    if (s && engine) engine.applyRemoteState(s, props.clockOffsetMs ?? 0);
  },
);
watch(
  () => props.mode,
  (m) => {
    if (m && engine && !props.eyeState) engine.setMode(m);
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
  <canvas ref="canvasRef" class="nb-eye-canvas" :class="{ passive: !interactive }" />
</template>

<style scoped>
.nb-eye-canvas { width: 100%; height: 100%; display: block; cursor: crosshair; touch-action: none; }
.nb-eye-canvas.passive { cursor: inherit; pointer-events: none; }
</style>
