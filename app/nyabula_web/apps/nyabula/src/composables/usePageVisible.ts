/* One shared "is this tab visible" flag. Polling composables stop asking the
 * device while the tab is hidden and refresh once when it comes back. */
import { onBeforeUnmount, readonly, ref, watch } from 'vue';

const visible = ref(typeof document === 'undefined' || document.visibilityState !== 'hidden');
if (typeof document !== 'undefined') {
  document.addEventListener('visibilitychange', () => { visible.value = document.visibilityState !== 'hidden'; });
}

export function usePageVisible() {
  return readonly(visible);
}

/** Call `poll` now and every `intervalMs` while the tab is visible. */
export function useVisiblePoll(poll: () => void, intervalMs: number): void {
  let timer: ReturnType<typeof setInterval> | undefined;
  watch(visible, shown => {
    if (timer) clearInterval(timer);
    timer = undefined;
    if (!shown) return;
    poll();
    timer = setInterval(poll, intervalMs);
  }, { immediate: true });
  onBeforeUnmount(() => { if (timer) clearInterval(timer); });
}
