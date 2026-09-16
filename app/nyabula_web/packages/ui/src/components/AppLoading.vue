<script setup lang="ts">
/* First-paint overlay: label + progress bar, exits with a circular clip
 * shrink once the host marks boot done (ported from Myself). */
import { ref, watch } from 'vue';
import { useLoadingStore } from '../stores/loading';
import NyabulaLogo from './NyabulaLogo.vue';

const loading = useLoadingStore();
const fadeOut = ref(false);
const removed = ref(false);

watch(
  () => loading.bootDone,
  (done) => {
    if (!done) return;
    window.setTimeout(() => {
      fadeOut.value = true;
      window.setTimeout(() => {
        removed.value = true;
        loading.bootOverlayVisible = false;
      }, 1100);
    }, 200);
  },
  { immediate: true },
);
</script>

<template>
  <div v-if="!removed" class="boot" :class="{ 'fade-out': fadeOut }" aria-live="polite">
    <div class="boot-content">
      <div class="boot-mark"><NyabulaLogo :size="96" /></div>
      <div class="boot-text">{{ loading.bootLabel }}</div>
      <div class="boot-bar">
        <div class="boot-bar-fill" :style="{ width: loading.bootProgress + '%' }" />
      </div>
    </div>
  </div>
</template>

<style scoped>
.boot {
  position: fixed;
  inset: 0;
  z-index: 9999;
  background: var(--md-surface);
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
  clip-path: circle(200% at 50% 50%);
}
.boot.fade-out {
  animation: boot-clip-shrink 1.05s var(--ease-inout, cubic-bezier(0.65, 0, 0.35, 1)) forwards;
}
@keyframes boot-clip-shrink {
  0% { clip-path: circle(200% at 50% 50%); }
  100% { clip-path: circle(0% at 50% 50%); }
}
.boot-content {
  width: min(420px, 80%);
  text-align: center;
}
.boot-mark {
  display: flex;
  justify-content: center;
  margin-bottom: 22px;
  animation: boot-fade-in 0.6s var(--ease-spring) 0.05s both;
}
.boot-text {
  font-family: var(--font-title);
  font-size: 22px;
  color: var(--md-on-surface);
  margin-bottom: 18px;
  animation: boot-fade-in 0.6s var(--ease-spring) 0.15s both;
}
.boot-bar {
  height: 4px;
  width: 40%;
  margin: 0 auto;
  border-radius: 999px;
  background: var(--md-surface-container-highest);
  overflow: hidden;
  animation: boot-fade-in 0.45s var(--ease-spring) 0.2s both;
}
.boot-bar-fill {
  height: 100%;
  width: 0%;
  border-radius: inherit;
  background: linear-gradient(90deg, var(--md-primary), var(--md-primary-deep));
  box-shadow: none;
  transition: width 0.25s ease-out;
}
@keyframes boot-fade-in {
  from { opacity: 0; transform: scale(0.85) translateY(10px); }
  to { opacity: 1; transform: scale(1) translateY(0); }
}
</style>
