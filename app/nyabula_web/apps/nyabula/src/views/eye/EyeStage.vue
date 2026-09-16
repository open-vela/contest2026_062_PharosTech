<script setup lang="ts">
/* The eye canvas + pairing overlay + placeholder, shared by all variants. */
import { EyeCanvas, PairOverlay, UiIcon } from '@nyabula/ui';
import { ref, watch } from 'vue';
import type { EyeEngine } from '@nyabula/eye-engine';
import { useEyeStore } from '../../stores/eye';
import { useSessionStore } from '../../stores/session';

defineProps<{
  ready: boolean;
  showPair: boolean;
  pairing: boolean;
  pairError: string | null;
  round?: boolean;
}>();
const emit = defineEmits<{
  (e: 'ready', engine: EyeEngine): void;
  (e: 'look', d: { x?: number; y?: number; release?: boolean }): void;
  (e: 'pair', code: string): void;
}>();
const eye = useEyeStore();
const session = useSessionStore();
const toyMode = ref(false);
watch(() => session.canControl, allowed => { if (!allowed) toyMode.value = false; });
function toggleToy() {
  toyMode.value = !toyMode.value;
  if (!toyMode.value) emit('look', { release: true });
}
</script>

<template>
  <div class="stage" :class="{ round }">
    <EyeCanvas
      v-if="ready"
      :eye-state="eye.lastState"
      :clock-offset-ms="eye.nativeCore ? 0 : session.clockOffsetMs()"
      :toy-mode="toyMode && session.canControl"
      @ready="emit('ready', $event)"
      @look="emit('look', $event)"
    />
    <div v-else class="placeholder">
      <UiIcon name="visibility" :size="42" />
      <p>{{ session.state === 'pairing-required' ? '输入设备屏幕上的配对码' : session.connected ? '眼睛服务未启动' : '等待连接…' }}</p>
    </div>
    <PairOverlay v-if="showPair" :busy="pairing" :error="pairError" @submit="emit('pair', $event)" />
    <button v-if="ready" class="toy-toggle" :aria-pressed="toyMode" :disabled="!session.canControl" @click="toggleToy">逗猫棒 {{ toyMode ? '开启' : '关闭' }}</button>
    <div v-if="ready" class="stage-hint">{{ toyMode ? '移动光标 / 手指点击拖动，让猫追着看' : '拖动画布 = 注视点' }}</div>
  </div>
</template>

<style scoped>
.stage {
  position: relative;
  width: 100%;
  height: 100%;
  min-height: 240px;
  border-radius: var(--radius-l);
  background: #000;
  overflow: hidden;
  box-shadow: inset 0 0 0 1px var(--md-outline-variant);
}
.stage.round { border-radius: 50%; aspect-ratio: 1; }
.toy-toggle { position: absolute; top: 12px; right: 12px; padding: 7px 12px; border: 1px solid var(--md-outline-variant); border-radius: var(--radius-s); background: var(--md-surface-container-high); color: var(--md-on-surface); cursor: pointer; }
.toy-toggle[aria-pressed="true"] { background: var(--md-primary-container); border-color: var(--md-primary); color: var(--md-on-primary-container); }
.toy-toggle:disabled { opacity: .4; cursor: default; }
.stage :deep(.eye-canvas) { width: 100%; height: 100%; border-radius: inherit; }
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
  bottom: 10px;
  transform: translateX(-50%);
  font-size: 11.5px;
  color: var(--md-on-surface);
  background: var(--md-surface-container-high);
  white-space: nowrap;
  max-width: calc(100% - 24px);
  backdrop-filter: blur(8px);
  padding: 3px 10px;
  border-radius: 999px;
  pointer-events: none;
  opacity: 0.8;
}
</style>
