<script setup lang="ts">
/* The eye canvas + pairing overlay + placeholder, shared by all variants.
 * The stage has no backdrop of its own: the canvas is transparent and only the
 * two round eyes are painted, so they float over the page. Its height follows
 * its width (see .stage) instead of a viewport share, which is what used to
 * leave tall empty bands above and below the eyes. */
import { EyeCanvas, PairOverlay, UiIcon } from '@nyabula/ui';
import type { EyeEngine } from '@nyabula/eye-engine';
import { useEyeStore } from '../../stores/eye';
import { useSessionStore } from '../../stores/session';

defineProps<{
  ready: boolean;
  showPair: boolean;
  pairing: boolean;
  pairError: string | null;
}>();
const emit = defineEmits<{
  (e: 'ready', engine: EyeEngine): void;
  (e: 'look', d: { x?: number; y?: number; release?: boolean }): void;
  (e: 'pair', code: string): void;
}>();
const eye = useEyeStore();
const session = useSessionStore();
</script>

<template>
  <div class="stage" :class="{ pairing: showPair }">
    <EyeCanvas
      v-if="ready"
      :eye-state="eye.lastState"
      :clock-offset-ms="eye.nativeCore ? 0 : session.clockOffsetMs()"
      @ready="emit('ready', $event)"
      @look="emit('look', $event)"
    />
    <div v-else class="placeholder">
      <UiIcon name="visibility" :size="42" />
      <p>{{ session.state === 'pairing-required' ? '输入设备屏幕上的配对码' : session.connected ? '眼睛服务未启动' : '等待连接…' }}</p>
    </div>
    <PairOverlay v-if="showPair" :busy="pairing" :error="pairError" @submit="emit('pair', $event)" />
    <div v-if="ready" class="stage-hint">拖动画布 = 注视点</div>
  </div>
</template>

<style scoped>
/* The engine lays the eyes out as: panel radius P = min(0.16 W, 0.30 H), eye
 * centres at 0.46 H. At W:H = 15:8 both limits meet, so the eyes are as large
 * as the width allows with 0.16 H of air above and 0.24 H below (the lower
 * band also carries the hint). max-height only bites on very wide columns,
 * where the eyes then centre horizontally instead of growing further. */
.stage {
  position: relative;
  flex: none;
  width: 100%;
  aspect-ratio: 15 / 8;
  min-height: 170px;
  max-height: var(--eye-stage-max-height, 560px);
  background: transparent;
}
.stage.pairing { min-height: 280px; }
.stage :deep(.eye-canvas) { width: 100%; height: 100%; }
.placeholder {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 10px;
  color: var(--md-on-surface-variant);
  font-size: 14px;
}
.placeholder p { margin: 0; }
.stage-hint {
  position: absolute;
  left: 50%;
  bottom: 2px;
  transform: translateX(-50%);
  font-size: 11.5px;
  color: var(--md-on-surface-variant);
  white-space: nowrap;
  max-width: calc(100% - 24px);
  pointer-events: none;
}
</style>
