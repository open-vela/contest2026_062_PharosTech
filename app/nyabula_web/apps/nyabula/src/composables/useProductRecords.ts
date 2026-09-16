import { computed, onBeforeUnmount, watch } from 'vue';
import { useProductStore, type ProductDomain, type ProductRecord } from '../stores/product';

/** Polling only refreshes a view. Core jobs outlive this composable. */
export function useProductRecords<T extends ProductRecord>(domain: ProductDomain, intervalMs = 1500) {
  const product = useProductStore();
  const items = computed(() => (product.records[domain]?.items ?? []) as T[]);
  const busy = computed(() => product.busy[domain] === true);
  const available = computed(() => product.available);
  const error = computed(() => product.errors[domain] ?? '');
  const receivedAt = computed(() => product.records[domain]?.receivedAt ?? performance.now());
  const revision = computed(() => product.records[domain]?.revision ?? 0);
  const loaded = computed(() => product.records[domain] !== undefined);
  let timer: ReturnType<typeof setInterval> | undefined;
  watch(available, ready => {
    if (timer) clearInterval(timer);
    timer = undefined;
    if (!ready) return;
    void product.refresh(domain);
    timer = setInterval(() => {
      if (!busy.value) void product.refresh(domain, true);
    }, intervalMs);
  }, { immediate:true });
  onBeforeUnmount(() => { if (timer) clearInterval(timer); });
  const mutate = (operation:string, data:Record<string,unknown>, expectedRevision?:number) => product.mutate(domain, operation, data, expectedRevision);
  return { items, busy, available, error, receivedAt, revision, loaded, mutate, refresh:() => product.refresh(domain) };
}
