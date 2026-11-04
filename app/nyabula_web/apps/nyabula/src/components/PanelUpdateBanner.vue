<script setup lang="ts">
/* "The device has a newer panel than this tab is running."  Device build
 * only.  Checked when the link comes back (an update reboots the device),
 * when the tab is looked at again, and every few minutes; never reloads on
 * its own, because the owner may be in the middle of typing something. */
import { onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { UiIcon } from '@nyabula/ui';
import { useSessionStore } from '../stores/session';
import { entryOf, isStale, runningEntry } from '../lib/panelVersion';

const CHECK_MS = 5 * 60 * 1000;
const session = useSessionStore();
const stale = ref(false);
let timer: number | undefined;
let checking = false;

async function check(): Promise<void> {
  if (!__NYA_DEVICE__ || stale.value || checking || document.hidden) return;
  checking = true;
  try {
    const response = await fetch(`${import.meta.env.BASE_URL}index.html`, { cache: 'no-store' });
    if (response.ok) stale.value = isStale(runningEntry(document.scripts), entryOf(await response.text()));
  } catch { /* offline: the connection banner says so */ } finally { checking = false; }
}
function onVisibility(): void { if (!document.hidden) void check(); }

watch(() => session.connected, connected => { if (connected) void check(); });
onMounted(() => {
  void check();
  timer = window.setInterval(() => void check(), CHECK_MS);
  document.addEventListener('visibilitychange', onVisibility);
});
onBeforeUnmount(() => {
  window.clearInterval(timer);
  document.removeEventListener('visibilitychange', onVisibility);
});
function reload(): void { window.location.reload(); }
</script>

<template>
  <Transition name="fade">
    <div v-if="stale" class="panel-update" role="status">
      <UiIcon name="sync" :size="18" />
      <span>面板已更新，刷新即可使用新版。</span>
      <button class="panel-update__reload" type="button" @click="reload">刷新</button>
    </div>
  </Transition>
</template>

<style scoped>
.panel-update {
  display: flex;
  flex: none;
  align-items: center;
  gap: 12px;
  margin: 8px 12px;
  padding: 10px 12px;
  border: 1px solid var(--md-outline-variant);
  border-radius: var(--radius-m, 12px);
  font: 400 14px/1.5 var(--font-body);
  background: var(--md-surface-container-high);
  color: var(--md-on-surface);
}
.panel-update > :first-child { flex: none; color: var(--md-primary); }
.panel-update span { flex: 1; min-width: 0; overflow-wrap: anywhere; }
.panel-update__reload {
  flex: none;
  border: 0;
  border-radius: var(--radius-s, 8px);
  padding: 8px 16px;
  min-height: 44px;
  font: 600 14px var(--font-body);
  cursor: pointer;
  background: var(--md-primary);
  color: var(--md-on-primary);
}
.panel-update__reload:focus-visible { outline: 2px solid var(--md-primary); outline-offset: 3px; }
:global([data-skin='nebula'] .panel-update) {
  background: var(--nb-glass-strong);
  border-color: var(--nb-line-strong);
  border-radius: 18px;
  backdrop-filter: var(--nb-blur);
  box-shadow: inset 0 1px 0 var(--nb-hi);
}
:global([data-skin='instrument'] .panel-update) { border-radius: 2px; border-left: 3px solid var(--md-primary); }
:global([data-skin='instrument'] .panel-update__reload) { border-radius: 2px; }
@media (max-width: 480px) {
  .panel-update { margin: 6px 10px; padding: 8px 10px; gap: 8px; }
}
.fade-enter-active, .fade-leave-active { transition: opacity var(--dur-fast); }
.fade-enter-from, .fade-leave-to { opacity: 0; }
@media (prefers-reduced-motion: reduce) {
  .fade-enter-active, .fade-leave-active { transition: none; }
}
</style>
