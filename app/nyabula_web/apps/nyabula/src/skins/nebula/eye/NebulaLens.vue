<script setup lang="ts">
/* Round-cornered black "lens" with a slow conic glow ring (design-lab A).
 * Same contract as views/eye/EyeStage.vue: live EyeCanvas (drag = gaze,
 * throttled look events), toy mode toggle for controllers, placeholder,
 * pairing overlay. */
import EyeMirrorCard from '../../../views/eye/EyeMirrorCard.vue';
import { ref, watch } from 'vue';
import { EyeCanvas, PairOverlay, UiIcon } from '@nyabula/ui';
import type { EyeEngine } from '@nyabula/eye-engine';
import { useEyeStore } from '../../../stores/eye';
import { useSessionStore } from '../../../stores/session';

defineProps<{ ready: boolean; showPair: boolean; pairing: boolean; pairError: string | null }>();
const emit = defineEmits<{
  (e: 'ready', engine: EyeEngine): void;
  (e: 'look', d: { x?: number; y?: number; release?: boolean }): void;
  (e: 'pair', code: string): void;
}>();
const eye = useEyeStore();
const session = useSessionStore();
const toyMode = ref(false);
watch(
  () => session.canControl,
  (allowed) => {
    if (!allowed) toyMode.value = false;
  },
);
function toggleToy() {
  toyMode.value = !toyMode.value;
  if (!toyMode.value) emit('look', { release: true });
}
</script>

<template>
  <div class="nb-lens">
    <div class="ring" />
    <div class="inner">
      <EyeMirrorCard v-if="ready && eye.nativeQr" embedded auto-start />
      <EyeCanvas
        v-else-if="ready"
        :eye-state="eye.lastState"
        :clock-offset-ms="eye.nativeCore ? 0 : session.clockOffsetMs()"
        :toy-mode="toyMode && session.canControl"
        @ready="emit('ready', $event)"
        @look="emit('look', $event)"
      />
      <div v-else class="placeholder">
        <span class="sleep"><i /><i /></span>
        <p>{{ session.state === 'pairing-required' ? '输入设备屏幕上的配对码' : session.connected ? '眼睛服务未启动' : '等待连接…' }}</p>
      </div>
      <PairOverlay v-if="showPair" :busy="pairing" :error="pairError" @submit="emit('pair', $event)" />
      <button v-if="ready && !eye.nativeQr" class="toy" :class="{ on: toyMode }" :aria-pressed="toyMode" :disabled="!session.canControl" @click="toggleToy">
        <UiIcon name="pets" :size="15" /> 逗猫棒 {{ toyMode ? '开' : '关' }}
      </button>
      <div v-if="ready && !eye.nativeQr" class="hint">{{ toyMode ? '移动光标 / 手指拖动，让猫追着看' : '拖动画面 = 注视点' }}</div>
    </div>
  </div>
</template>

<style scoped>
.nb-lens { position: relative; width: 100%; height: 100%; min-height: 220px; }
.ring {
  position: absolute;
  inset: -2px;
  border-radius: 48px;
  background: conic-gradient(from 0deg, rgba(var(--nb-acc-rgb), 0.5), rgba(var(--nb-violet-rgb), 0.5), rgba(255, 143, 177, 0.4), rgba(var(--nb-acc-rgb), 0.5));
  filter: blur(18px);
  opacity: 0.55;
  animation: nb-rot 12s linear infinite;
}
@keyframes nb-rot { to { transform: rotate(360deg); } }
.inner {
  position: absolute;
  inset: 0;
  border-radius: 46px;
  overflow: hidden;
  background: #000;
  border: 1px solid rgba(255, 255, 255, 0.12);
  box-shadow: inset 0 0 60px rgba(0, 0, 0, 0.9), 0 40px 80px -30px rgba(0, 0, 0, 0.7);
}
.inner :deep(.eye-canvas) { width: 100%; height: 100%; border-radius: 0; }
.placeholder { position: absolute; inset: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 18px; color: rgba(220, 226, 255, 0.6); font-size: 13.5px; letter-spacing: 0.06em; }
.placeholder p { margin: 0; }
.sleep { display: flex; gap: 46px; }
.sleep i { width: 70px; height: 6px; border-radius: 6px; background: rgba(var(--nb-acc-rgb), 0.55); box-shadow: 0 0 18px rgba(var(--nb-acc-rgb), 0.6); animation: nb-snooze 3.6s ease-in-out infinite; }
.sleep i:last-child { animation-delay: 0.15s; }
@keyframes nb-snooze { 50% { opacity: 0.45; transform: scaleX(0.9); } }
.toy {
  position: absolute;
  top: 14px;
  right: 14px;
  display: inline-flex;
  align-items: center;
  gap: 5px;
  padding: 7px 12px;
  border-radius: 999px;
  border: 1px solid rgba(255, 255, 255, 0.14);
  background: rgba(20, 26, 56, 0.55);
  -webkit-backdrop-filter: blur(10px);
  backdrop-filter: blur(10px);
  color: rgba(236, 240, 255, 0.9);
  font: 600 12.5px var(--font-body);
  cursor: pointer;
  transition: background 0.2s, border-color 0.2s, transform 0.35s var(--nb-spring);
}
.toy:active:not(:disabled) { transform: scale(0.95); }
.toy.on { background: rgba(var(--nb-acc-rgb), 0.25); border-color: rgba(var(--nb-acc-rgb), 0.6); color: #fff; }
.toy:disabled { opacity: 0.4; cursor: default; }
.hint {
  position: absolute;
  left: 50%;
  bottom: 14px;
  transform: translateX(-50%);
  padding: 4px 12px;
  border-radius: 999px;
  background: rgba(20, 26, 56, 0.5);
  color: rgba(236, 240, 255, 0.75);
  font-size: 11.5px;
  white-space: nowrap;
  max-width: calc(100% - 24px);
  overflow: hidden;
  text-overflow: ellipsis;
  pointer-events: none;
}
@media (max-width: 720px) {
  .ring { border-radius: 34px; }
  .inner { border-radius: 32px; }
}
</style>
