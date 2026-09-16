/* Core-owned product records. Local values are caches/drafts, never jobs. */
import { computed, ref, watch } from 'vue';
import { defineStore } from 'pinia';
import { useToastStore } from '@nyabula/ui';
import { useSessionStore } from './session';

export type ProductDomain = 'memory' | 'task' | 'calendar' | 'timer' | 'alarm';
export interface ProductRecord { id: string; [key: string]: unknown }
export interface ProductSnapshot { revision: number; items: ProductRecord[]; uptime_ms?: number; receivedAt: number }
const DOMAINS: ProductDomain[] = ['memory', 'task', 'calendar', 'timer', 'alarm'];

function snapshot(value: Record<string, unknown>, domain: ProductDomain): ProductSnapshot {
  if (!Number.isSafeInteger(value.revision) || Number(value.revision) < 0 || !Array.isArray(value.items)
      || !value.items.every(r => r && typeof r === 'object' && typeof r.id === 'string'
        && (domain !== 'memory' || typeof r.text === 'string')
        && (domain !== 'task' || (typeof r.title === 'string' && typeof r.state === 'string'))
        && (domain !== 'calendar' || (typeof r.title === 'string' && Number.isFinite(r.start_at)))
        && (domain !== 'timer' || (typeof r.kind === 'string' && typeof r.status === 'string'
          && Number.isFinite(r.remaining_ms) && Number.isFinite(r.elapsed_ms))))) {
    throw new Error('设备返回的记录格式无效');
  }
  return { revision:Number(value.revision), items:value.items as ProductRecord[],
    uptime_ms:typeof value.uptime_ms === 'number' ? value.uptime_ms : undefined, receivedAt:performance.now() };
}

export const useProductStore = defineStore('product', () => {
  const session = useSessionStore();
  const toast = useToastStore();
  const records = ref<Partial<Record<ProductDomain, ProductSnapshot>>>({});
  const busy = ref<Partial<Record<ProductDomain, boolean>>>({});
  const errors = ref<Partial<Record<ProductDomain, string>>>({});
  const available = computed(() => session.connected && session.client?.capabilities.includes('core.product-v1') === true);
  let generation = 0;
  let off: (() => void) | null = null;
  const refreshing = new Map<ProductDomain, Promise<void>>();

  watch(() => session.client, client => {
    generation++;
    off?.();
    records.value = {};
    busy.value = {};
    errors.value = {};
    refreshing.clear();
    off = client?.on('product.changed', data => {
      if (DOMAINS.includes(data.domain as ProductDomain)) void refresh(data.domain as ProductDomain, true);
    }) ?? null;
  }, { immediate:true });

  watch(() => session.state, state => {
    if (state === 'connected') return;
    generation++;
    records.value = {};
    busy.value = {};
    errors.value = {};
    refreshing.clear();
  });

  function refresh(domain: ProductDomain, silent = false): Promise<void> {
    if (!available.value) return Promise.resolve();
    const existing = refreshing.get(domain);
    if (existing) return existing;
    const current = generation;
    const request = session.request(`${domain}.list`).then(value => {
      if (current !== generation) return;
      const parsed = snapshot(value, domain);
      if ((records.value[domain]?.revision ?? -1) <= parsed.revision) records.value[domain] = parsed;
      delete errors.value[domain];
    }).catch(error => {
      if (current !== generation) return;
      errors.value[domain] = error instanceof Error ? error.message : String(error);
      if (!silent) toast.error(error, '读取设备记录失败');
    }).finally(() => {
      if (current === generation) refreshing.delete(domain);
    });
    refreshing.set(domain, request);
    return request;
  }

  async function mutate(domain: ProductDomain, operation: string, data: Record<string, unknown>, expectedRevision?: number): Promise<boolean> {
    if (!available.value || !session.canControl) return false;
    if (busy.value[domain]) { toast.warn('正在保存，请稍候'); return false; }
    const current = generation;
    busy.value[domain] = true;
    try {
      if (!records.value[domain]) await refresh(domain);
      const base = records.value[domain];
      if (!base || current !== generation) return false;
      const result = await session.request(`${domain}.${operation}`, { ...data, revision:expectedRevision ?? base.revision });
      if (current !== generation) return false;
      records.value[domain] = snapshot(result, domain);
      delete errors.value[domain];
      return true;
    } catch (error) {
      if (current !== generation) return false;
      const conflict = (error as { code?:string }).code === 'ECONFLICT';
      errors.value[domain] = conflict ? '其他客户端已更新数据，请确认后重试' : error instanceof Error ? error.message : String(error);
      toast.error(errors.value[domain], '保存失败');
      await refresh(domain, true);
      return false;
    } finally {
      if (current === generation) busy.value[domain] = false;
    }
  }

  return { records, busy, errors, available, refresh, mutate };
});
