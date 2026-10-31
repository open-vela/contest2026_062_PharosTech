/* Per-device stats cache (GET /devices/{id}/stats). Shared by the
 * overview (aggregated charts), the fleet list (clients, 30-day online
 * rate) and the device detail. All derived metrics come from these real
 * rows: daily {date, onlineSeconds, framesUp, framesDown} + current
 * {online, clients}. */
import { defineStore } from 'pinia';
import { ref } from 'vue';
import { deviceStats, type DailyStat, type DeviceStats } from '../api/client';
import { lastDays } from '../logic/format';

export const useStatsStore = defineStore('stats', () => {
  const byId = ref<Record<string, DeviceStats>>({});
  const errors = ref<Record<string, string>>({});
  const loading = ref(false);
  let generation = 0;
  function reset() {
    generation++;
    byId.value = {};
    errors.value = {};
    loading.value = false;
  }

  async function load(id: string): Promise<DeviceStats | null> {
    const current = generation;
    try {
      const s = await deviceStats(id);
      if (current !== generation) return null;
      byId.value = { ...byId.value, [id]: s };
      const { [id]: _drop, ...rest } = errors.value;
      errors.value = rest;
      return s;
    } catch (e) {
      if (current !== generation) return null;
      errors.value = { ...errors.value, [id]: e instanceof Error ? e.message : String(e) };
      return null;
    }
  }

  async function loadAll(ids: string[]): Promise<void> {
    loading.value = true;
    try {
      await Promise.allSettled(ids.map(load));
    } finally {
      loading.value = false;
    }
  }

  function forget(id: string): void {
    const { [id]: _drop, ...rest } = byId.value;
    byId.value = rest;
  }

  /** Daily rows for the last `days` days (oldest first), zero-filled. The
   * API returns up to 90 rows newest-first; missing days had no activity. */
  function series(id: string, days = 30): DailyStat[] {
    const rows = new Map((byId.value[id]?.daily ?? []).map((d) => [d.date, d]));
    return lastDays(days).map((date) => rows.get(date) ?? { date, onlineSeconds: 0, framesUp: 0, framesDown: 0 });
  }

  /** Share of the last `days` days the device was online (0..1), from
   * summed daily onlineSeconds. */
  function onlineRate(id: string, days = 30): number | null {
    if (!byId.value[id]) return null;
    const total = series(id, days).reduce((n, d) => n + d.onlineSeconds, 0);
    return Math.min(1, total / (days * 86400));
  }

  return { byId, errors, loading, load, loadAll, forget, series, onlineRate, reset };
});
