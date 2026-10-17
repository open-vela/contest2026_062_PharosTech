<script setup lang="ts">
/* Route curtain: dim+blur, a primary color panel wipes down, spinner in the
 * middle; exit clips the whole thing downward (ported from Myself). */
import { computed, ref, watch } from 'vue';
import { useLoadingStore } from '../stores/loading';

const loading = useLoadingStore();
type Stage = 'idle' | 'enter' | 'leave';
const stage = ref<Stage>('idle');
const LEAVE_MS = 600;
/* dim 0.25s + panel 0.5s starting at 0.08s = fully covered at ~0.6s */
const ENTER_MS = 620;
let coverTimer: number | null = null;

watch(
  () => loading.routeLoading,
  (on) => {
    if (on) {
      stage.value = 'enter';
      if (coverTimer) window.clearTimeout(coverTimer);
      coverTimer = window.setTimeout(() => {
        coverTimer = null;
        if (stage.value === 'enter') loading.markCovered();
      }, ENTER_MS);
    } else if (stage.value === 'enter') {
      stage.value = 'leave';
      window.setTimeout(() => {
        if (stage.value === 'leave') stage.value = 'idle';
        loading.routeOverlayVisible = false;
      }, LEAVE_MS);
    }
  },
);
const visible = computed(() => stage.value !== 'idle');
</script>

<template>
  <Teleport to="body">
    <div v-if="visible" class="route-loading" :class="stage">
      <div class="dim" />
      <div class="panel" />
      <div class="indicator">
        <span class="spinner" />
        <span class="label">载入中</span>
      </div>
    </div>
  </Teleport>
</template>

<style scoped>
.route-loading {
  position: fixed;
  inset: 0;
  z-index: 9000;
  overflow: hidden;
  pointer-events: none;
}
.route-loading.leave {
  animation: wipe-away 0.6s var(--ease-inout) forwards;
}
@keyframes wipe-away {
  from { clip-path: inset(0 0 0 0); }
  to { clip-path: inset(100% 0 0 0); }
}
.dim {
  position: absolute;
  inset: 0;
  background: rgba(0, 0, 0, 0.3);
  backdrop-filter: blur(12px);
  -webkit-backdrop-filter: blur(12px);
}
.route-loading.enter .dim { animation: dim-in 0.25s ease both; }
@keyframes dim-in { from { opacity: 0; } to { opacity: 1; } }
.panel {
  position: absolute;
  inset: 0;
  background: linear-gradient(180deg, var(--md-primary-deep), var(--md-primary));
}
.route-loading.enter .panel { animation: panel-down 0.5s var(--ease-inout) 0.08s both; }
@keyframes panel-down { from { transform: translateY(-100%); } to { transform: translateY(0); } }
.indicator {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 16px;
  color: #fff;
}
.route-loading.enter .indicator { animation: dim-in 0.3s ease 0.35s both; }
.spinner {
  width: 40px;
  height: 40px;
  border-radius: 50%;
  border: 3px solid rgba(255, 255, 255, 0.3);
  border-top-color: #fff;
  animation: spin 0.8s linear infinite;
}
@keyframes spin { to { transform: rotate(360deg); } }
.label {
  font-family: var(--font-title);
  font-size: 16px;
  letter-spacing: 0.14em;
  animation: label-pulse 1.6s ease-in-out infinite;
}
@keyframes label-pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.6; } }
</style>
