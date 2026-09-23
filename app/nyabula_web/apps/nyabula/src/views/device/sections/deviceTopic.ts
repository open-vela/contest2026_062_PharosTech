/* One-topic readout for the maintenance sections (storage / update): like
 * useAsyncTask, but a device that does not know the topic (ENOTFOUND, older
 * firmware) ends in `unsupported` instead of an error toast. */
import { onMounted, ref, watch, type Ref } from 'vue';
import { useLoadingStore, useToastStore } from '@nyabula/ui';
import { useSessionStore } from '../../../stores/session';
import { isUnsupportedError } from '../../../lib/deviceMaint';

export function useDeviceTopic<T>(topic: string, parse: (raw: unknown) => T, errorPrefix: string) {
  const session = useSessionStore();
  const toast = useToastStore();
  const loading = useLoadingStore();
  const data = ref<T | null>(null) as Ref<T | null>;
  const busy = ref(false);
  const unsupported = ref(false);
  const failed = ref(false);

  async function run(opts: { holdRoute?: boolean } = {}): Promise<void> {
    if (busy.value || unsupported.value) return;
    busy.value = true;
    failed.value = false;
    const release = opts.holdRoute ? loading.holdRoute() : () => undefined;
    try {
      data.value = parse(await session.request(topic));
    } catch (e) {
      if (isUnsupportedError(e)) unsupported.value = true;
      else {
        failed.value = true;
        toast.error(e, errorPrefix);
      }
    } finally {
      busy.value = false;
      release();
    }
  }

  onMounted(() => void run({ holdRoute: true }));
  // A reconnect may land on a different firmware: ask again.
  watch(() => session.connected, (c, prev) => {
    if (!c || prev !== false) return;
    unsupported.value = false;
    void run();
  });

  return { session, data, busy, unsupported, failed, run };
}
