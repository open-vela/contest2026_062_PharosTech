import { computed, onBeforeUnmount, watch } from 'vue';
import { useProductStore, type ProductDomain, type ProductRecord } from '../stores/product';
import { usePageVisible } from './usePageVisible';

/* One poller per domain, shared by every mounted view of it: the feature list
 * mounts several cards on the same records, and each used to run its own
 * interval. The fastest subscriber sets the pace; nothing is asked while the
 * tab is hidden, and the records are re-read once when it comes back. (The
 * store also listens for `product.changed`, which Core does not emit yet.) */
interface Poller { intervals: number[]; timer?: ReturnType<typeof setInterval>; every: number }
const pollers = new Map<ProductDomain, Poller>();

function retime(domain: ProductDomain, refresh: () => void): void {
  const poller = pollers.get(domain);
  if (!poller) return;
  const every = poller.intervals.length ? Math.min(...poller.intervals) : 0;
  if (every === poller.every && (poller.timer !== undefined) === (every > 0)) return;
  if (poller.timer) clearInterval(poller.timer);
  poller.every = every;
  poller.timer = every > 0 ? setInterval(refresh, every) : undefined;
}

/** Polling only refreshes a view. Core jobs outlive this composable. */
export function useProductRecords<T extends ProductRecord>(domain: ProductDomain, intervalMs = 1500) {
  const product = useProductStore();
  const visible = usePageVisible();
  const items = computed(() => (product.records[domain]?.items ?? []) as T[]);
  const busy = computed(() => product.busy[domain] === true);
  const available = computed(() => product.available);
  const error = computed(() => product.errors[domain] ?? '');
  const receivedAt = computed(() => product.records[domain]?.receivedAt ?? performance.now());
  const revision = computed(() => product.records[domain]?.revision ?? 0);
  const loaded = computed(() => product.records[domain] !== undefined);

  const poll = (): void => { if (product.busy[domain] !== true) void product.refresh(domain, true); };
  let subscribed = false;
  function subscribe(on: boolean): void {
    if (on === subscribed) return;
    subscribed = on;
    let poller = pollers.get(domain);
    if (!poller) pollers.set(domain, poller = { intervals: [], every: 0 });
    if (on) poller.intervals.push(intervalMs);
    else poller.intervals.splice(poller.intervals.indexOf(intervalMs), 1);
    retime(domain, poll);
  }
  watch([available, visible], ([ready, shown]) => {
    const on = ready && shown;
    if (on && !subscribed) void product.refresh(domain, loaded.value);
    subscribe(on);
  }, { immediate:true });
  onBeforeUnmount(() => subscribe(false));
  const mutate = (operation:string, data:Record<string,unknown>, expectedRevision?:number) => product.mutate(domain, operation, data, expectedRevision);
  return { items, busy, available, error, receivedAt, revision, loaded, mutate, refresh:() => product.refresh(domain) };
}
