/* compute.status, asked at the pace the state deserves: every second while
 * the model is being moved, loaded or is answering (or a file is being pulled
 * into the compute domain), every ten seconds otherwise; not at all while the
 * tab is hidden, the device is away, or the view is gone.
 *
 * A firmware without a compute domain answers ENOTFOUND: `unsupported`, no
 * error, no more questions until the next connection. */
import { onBeforeUnmount, ref, watch, type Ref } from 'vue';
import { useSessionStore } from '../stores/session';
import { usePageVisible } from './usePageVisible';
import { errorCode, parseComputeStatus, pollIntervalMs, POLL_SLOW_MS, type ComputeStatus } from '../lib/deviceCompute';

const REQUEST_TIMEOUT_MS = 5000;

export function useComputeStatus() {
  const session = useSessionStore();
  const visible = usePageVisible();
  const status = ref<ComputeStatus | null>(null) as Ref<ComputeStatus | null>;
  const unsupported = ref(false);
  /** The last question went unanswered (not "unsupported"). */
  const failed = ref(false);
  const asking = ref(false);
  let timer: ReturnType<typeof setTimeout> | null = null;
  let alive = true;

  const canPoll = (): boolean => alive && visible.value && session.connected && !unsupported.value;

  function stop(): void {
    if (timer) clearTimeout(timer);
    timer = null;
  }

  function schedule(): void {
    stop();
    if (!canPoll()) return;
    const s = status.value;
    timer = setTimeout(() => void refresh(), s ? pollIntervalMs(s.llm.state, s.blob.active || s.blob.hashing) : POLL_SLOW_MS);
  }

  async function refresh(): Promise<void> {
    if (asking.value || !canPoll()) return;
    stop();
    const client = session.client;
    asking.value = true;
    try {
      const raw = await session.request('compute.status', {}, { timeoutMs: REQUEST_TIMEOUT_MS });
      if (client !== session.client) return;
      status.value = parseComputeStatus(raw);
      failed.value = false;
    } catch (e) {
      if (client !== session.client) return;
      if (errorCode(e) === 'ENOTFOUND') {
        unsupported.value = true;
        status.value = null;
      } else failed.value = true;
    } finally {
      asking.value = false;
      // An answer from the previous connection says nothing: ask the new one now.
      if (client !== session.client && canPoll()) void refresh();
      else schedule();
    }
  }

  /** The answer of compute.llm.load / unload is a status too: show it at once. */
  function accept(raw: unknown): void {
    if (typeof raw === 'object' && raw !== null && 'llm' in raw) {
      status.value = parseComputeStatus(raw);
      failed.value = false;
      schedule();
    } else void refresh();
  }

  watch([visible, () => session.connected, () => session.client], ([, connected, client], old) => {
    if (old && (client !== old[2] || (connected && !old[1]))) {
      // Another device, or the same one after a firmware update: ask again.
      unsupported.value = false;
      failed.value = false;
      if (client !== old[2]) status.value = null;
    }
    if (canPoll()) void refresh();
    else stop();
  }, { immediate: true });

  onBeforeUnmount(() => {
    alive = false;
    stop();
  });

  return { status, unsupported, failed, asking, refresh, accept };
}
