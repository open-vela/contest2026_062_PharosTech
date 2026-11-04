<script setup lang="ts">
import { EyeCanvas } from '@nyabula/ui';
import type { EyeEngine } from '@nyabula/eye-engine';
import { useEyeStore } from '../../../stores/eye';
import { useSessionStore } from '../../../stores/session';

defineProps<{ disabled: boolean }>();
const emit = defineEmits<{ (e: 'look', value: { x?: number; y?: number; release?: boolean }): void }>();
const eye = useEyeStore();
const session = useSessionStore();
function ready(engine: EyeEngine) {
  if (!eye.lastState?.expression) engine.setLight(eye.lightPreview / 100);
}
</script>

<template>
  <div class="calibration-eye-stage" :class="{ disabled }" aria-label="点击并保持注视点">
    <EyeCanvas
      v-if="eye.ready"
      :eye-state="eye.lastState"
      :clock-offset-ms="eye.nativeCore ? 0 : session.clockOffsetMs()"
      :toy-mode="false"
      :click-hold="true"
      toy-scope="canvas"
      @ready="ready"
      @look="emit('look', $event)"
    />
    <p v-else class="eye-unavailable">{{ session.connected ? '等待设备眼睛状态…' : '设备离线' }}</p>
  </div>
</template>

<style scoped>
.calibration-eye-stage { position:relative;width:min(100%,440px);aspect-ratio:2/1;margin-inline:auto;background:transparent;border:1px dashed var(--md-outline);border-radius:4px; }
.calibration-eye-stage :deep(.eye-canvas) { position:absolute;inset:0;width:100%;height:100%; }
.calibration-eye-stage.disabled :deep(.eye-canvas) { pointer-events:none; }
.eye-unavailable { position:absolute;inset:0;display:grid;place-items:center;margin:0;color:var(--md-on-surface-variant);font-size:14px; }
</style>
