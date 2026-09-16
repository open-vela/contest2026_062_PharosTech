/* Request helper: loading flag + toast on failure + route-curtain latch. */
import { onMounted, ref } from 'vue';
import { useLoadingStore, useToastStore } from '@nyabula/ui';

export function useAsyncTask<T>(fn: () => Promise<T>, opts: { errorPrefix?: string; holdRoute?: boolean; immediate?: boolean } = {}) {
  const toast = useToastStore();
  const loading = useLoadingStore();
  const busy = ref(false);
  const error = ref<unknown>(null);
  const data = ref<T | null>(null) as { value: T | null };

  async function run(): Promise<T | null> {
    busy.value = true;
    error.value = null;
    const release = opts.holdRoute ? loading.holdRoute() : () => undefined;
    try {
      data.value = await fn();
      return data.value;
    } catch (e) {
      error.value = e;
      if (opts.errorPrefix !== undefined) toast.error(e, opts.errorPrefix);
      return null;
    } finally {
      busy.value = false;
      release();
    }
  }

  if (opts.immediate) onMounted(() => void run());
  return { busy, error, data, run };
}
