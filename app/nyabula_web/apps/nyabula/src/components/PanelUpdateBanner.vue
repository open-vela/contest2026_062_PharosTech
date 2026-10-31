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
    <div v-if="stale" class="banner" role="status">
      <UiIcon name="sync" :size="18" />
      <span>设备上的面板已更新，这个页面还是旧版本。刷新后才能看到新的界面和修复。</span>
      <button class="retry" type="button" @click="reload">刷新</button>
    </div>
  </Transition>
</template>

<style scoped>
.banner {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 8px 16px;
  font-size: 13px;
  background: color-mix(in srgb, var(--md-primary) 18%, var(--md-surface-container));
  color: var(--md-on-surface);
}
.banner span { flex: 1; min-width: 0; }
.retry {
  flex: none;
  border: 0;
  border-radius: 999px;
  padding: 6px 14px;
  min-height: 32px;
  font: 600 13px var(--font-body);
  cursor: pointer;
  background: var(--md-primary);
  color: var(--md-on-primary);
}
.fade-enter-active, .fade-leave-active { transition: opacity var(--dur-fast); }
.fade-enter-from, .fade-leave-to { opacity: 0; }
</style>
