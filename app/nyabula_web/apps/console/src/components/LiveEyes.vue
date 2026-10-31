<script setup lang="ts">
/* Decorative local cat eyes (real EyeEngine, transparent background,
 * follows the pointer). Not connected to any device: `mode` is driven by
 * the page (e.g. the login form reacting to focus). */
import { onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { EyeEngine } from '@nyabula/eye-engine';

const props = withDefaults(defineProps<{ mode?: string; follow?: boolean }>(), { mode: 'idle', follow: true });

const cv = ref<HTMLCanvasElement | null>(null);
let engine: EyeEngine | null = null;

onMounted(() => {
  engine = new EyeEngine(cv.value!, { background: 'transparent', toyMode: props.follow, localLook: true });
  engine.setMode(props.mode);
  engine.start();
});
watch(() => props.mode, (m) => engine?.setMode(m));
onBeforeUnmount(() => {
  engine?.destroy();
  engine = null;
});
</script>

<template>
  <canvas ref="cv" class="live-eyes" aria-hidden="true" />
</template>

<style scoped>
.live-eyes { width: 100%; height: 100%; display: block; touch-action: none; }
</style>
