import { ref, watch } from 'vue';
import { describeError } from '@nyabula/ui';
import { useSessionStore } from '../stores/session';
import { useVisiblePoll } from './usePageVisible';

/** Compact cards share the same lifecycle rules as their full controls. */
export function useFeatureStatus<T>(topic: string) {
  const session = useSessionStore();
  const status = ref<T>();
  const error = ref(''), readError = ref(''), busy = ref(false);
  let revision = 0, reading = false;
  async function refresh() {
    if (!session.connected || reading || busy.value) return;
    reading = true;
    const version = revision, client = session.client;
    try {
      const next = await session.request(topic);
      if (version === revision && client === session.client) { status.value = next as T; readError.value = ''; }
    } catch (e) { if (version === revision && client === session.client) readError.value = describeError(e); }
    finally { reading = false; }
  }
  async function action(command: string, data: Record<string, unknown> = {}) {
    if (!session.connected || !session.isOwner || busy.value) return;
    busy.value = true; error.value = '';
    const version = ++revision, client = session.client;
    try { await session.request(command, data); }
    catch (e) { if (version === revision && client === session.client) error.value = describeError(e); }
    finally { if (version === revision) { busy.value = false; void refresh(); } }
  }
  watch(() => session.client, () => { revision++; busy.value = false; status.value = undefined; error.value = ''; readError.value = ''; void refresh(); });
  useVisiblePoll(() => void refresh(), 2500);
  return { session, status, busy, error, readError, action };
}
