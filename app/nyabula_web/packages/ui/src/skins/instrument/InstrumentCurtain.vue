<script setup lang="ts">
/* Instrument route curtain: a mechanical shutter. Horizontal slats drop
 * shut top-down in quick stepped succession, a dot-matrix LOADING readout
 * with a scanning LED bar sits on the closed shutter, then the slats
 * fade out top-down, continuing the entry direction. Contract (see ClassicCurtain.vue): markCovered() once
 * the old page is fully hidden; clear routeOverlayVisible after the exit. */
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { useLoadingStore } from '../../stores/loading';
import DotMatrix from './DotMatrix.vue';

const loading = useLoadingStore();
type Stage = 'idle' | 'enter' | 'leave';
const stage = ref<Stage>('idle');
const SLATS = 8;
const STAGGER = 34;
const SLAT_MS = 220;
/* Last slat starts at (SLATS-1)*STAGGER and takes SLAT_MS => ~460ms. */
const ENTER_MS = (SLATS - 1) * STAGGER + SLAT_MS + 40;
const LEAVE_MS = (SLATS - 1) * STAGGER + SLAT_MS + 20;
let coverTimer: number | null = null;
let leaveTimer: number | null = null;
onBeforeUnmount(() => {
  if (coverTimer !== null) window.clearTimeout(coverTimer);
  if (leaveTimer !== null) window.clearTimeout(leaveTimer);
});

watch(
  () => loading.routeLoading,
  (on) => {
    if (on) {
      if (leaveTimer !== null) window.clearTimeout(leaveTimer);
      leaveTimer = null;
      stage.value = 'enter';
      if (coverTimer) window.clearTimeout(coverTimer);
      coverTimer = window.setTimeout(() => {
        coverTimer = null;
        if (stage.value === 'enter') loading.markCovered();
      }, ENTER_MS);
    } else if (stage.value === 'enter') {
      stage.value = 'leave';
      leaveTimer = window.setTimeout(() => {
        leaveTimer = null;
        if (stage.value !== 'leave') return;
        stage.value = 'idle';
        loading.routeOverlayVisible = false;
      }, LEAVE_MS);
    }
  },
);
const visible = computed(() => stage.value !== 'idle');
</script>

<template>
  <Teleport to="body">
    <div v-if="visible" class="ins-curtain" :class="stage" :style="{ '--n': SLATS, '--stagger': STAGGER + 'ms', '--slat': SLAT_MS + 'ms' }">
      <span v-for="i in SLATS" :key="i" class="slat" :style="{ '--i': i - 1 }" />
      <div class="readout">
        <DotMatrix text="LOADING" :dot="4" :gap="1.6" />
        <div class="scan"><i /></div>
        <span class="sub">载入中</span>
      </div>
    </div>
  </Teleport>
</template>

<style scoped>
.ins-curtain {
  position: fixed;
  inset: 0;
  z-index: 9000;
  pointer-events: none;
  display: flex;
  flex-direction: column;
  color: var(--md-on-surface);
}
.slat {
  flex: 1;
  background: var(--md-surface-container);
  border-bottom: 1px solid var(--md-outline-variant);
  transform-origin: 50% 0;
  transform: scaleY(0);
}
.slat:last-of-type { border-bottom: none; }
.enter .slat { animation: slat-shut var(--slat) cubic-bezier(0.22, 1, 0.36, 1) calc(var(--i) * var(--stagger)) forwards; }
.leave .slat {
  transform: scaleY(1.02);
  animation: slat-fade var(--slat) ease-out calc(var(--i) * var(--stagger)) forwards;
}
@keyframes slat-shut { from { transform: scaleY(0); } to { transform: scaleY(1.02); } }
@keyframes slat-fade { from { opacity: 1; } to { opacity: 0; } }

.readout {
  position: absolute;
  left: 50%;
  top: 50%;
  transform: translate(-50%, -50%);
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 12px;
  color: var(--md-on-surface);
  opacity: 0;
}
.enter .readout { animation: ro-in 0.18s steps(3, jump-none) 0.3s forwards; }
.leave .readout { opacity: 1; animation: ro-out 0.12s steps(2, jump-none) forwards; }
@keyframes ro-in { to { opacity: 1; } }
@keyframes ro-out { to { opacity: 0; } }
.scan { position: relative; width: 132px; height: 3px; background: var(--md-outline-variant); overflow: hidden; }
.scan i {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 28%;
  background: var(--md-primary);
  box-shadow: 0 0 8px rgba(var(--md-primary-rgb), 0.8);
  animation: sweep 0.9s steps(12, jump-none) infinite alternate;
}
@keyframes sweep { from { transform: translateX(0); } to { transform: translateX(260%); } }
.sub {
  font: 600 10.5px ui-monospace, 'Cascadia Mono', Consolas, monospace;
  letter-spacing: 0.3em;
  color: var(--md-on-surface-variant);
}
</style>
