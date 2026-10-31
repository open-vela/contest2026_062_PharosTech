/* Overview data: GET /stats/overview (KPIs) + the device list + every
 * device's daily stats, aggregated client-side into account-wide series.
 * Derived (not served by the API as such): 30-day totals and the daily
 * aggregate are sums of /devices/{id}/stats daily rows across devices. */
import { computed, onMounted, ref } from 'vue';
import { statsOverview, type DailyStat, type Overview } from '../api/client';
import { useDevicesStore } from '../stores/devices';
import { useStatsStore } from '../stores/stats';
import { lastDays } from './format';

export function useOverview(days = 30) {
  const devices = useDevicesStore();
  const stats = useStatsStore();
  const overview = ref<Overview | null>(null);
  const error = ref<string | null>(null);
  const loading = ref(false);
  const loaded = ref(false);

  async function refresh(): Promise<void> {
    loading.value = true;
    error.value = null;
    try {
      const [ov] = await Promise.all([
        statsOverview().catch((e: unknown) => {
          error.value = e instanceof Error ? e.message : String(e);
          return null;
        }),
        devices.refresh(),
      ]);
      overview.value = ov;
      await stats.loadAll(devices.devices.map((d) => d.deviceId));
      if (devices.lastError) error.value = devices.lastError;
      const incomplete = devices.devices.filter(d => stats.errors[d.deviceId]).length;
      if (incomplete) error.value = `${incomplete} 台设备的统计读取失败，合计不完整；请刷新重试。`;
    } finally {
      loading.value = false;
      loaded.value = true;
    }
  }
  onMounted(() => void refresh());

  /** Account-wide daily series, oldest first (sum across devices). */
  const daily = computed<DailyStat[]>(() => {
    const per = devices.devices.map((d) => stats.series(d.deviceId, days));
    return lastDays(days).map((date, i) => {
      const row: DailyStat = { date, onlineSeconds: 0, framesUp: 0, framesDown: 0 };
      for (const s of per) {
        const d = s[i];
        if (!d) continue;
        row.onlineSeconds += d.onlineSeconds;
        row.framesUp += d.framesUp;
        row.framesDown += d.framesDown;
      }
      return row;
    });
  });
  const frames30 = computed(() => daily.value.reduce((n, d) => n + d.framesUp + d.framesDown, 0));
  const onlineHours30 = computed(() => daily.value.reduce((n, d) => n + d.onlineSeconds, 0) / 3600);
  const hasTraffic = computed(() => daily.value.some((d) => d.framesUp + d.framesDown > 0 || d.onlineSeconds > 0));

  /** Devices: online first, then most recently seen. */
  const recent = computed(() =>
    [...devices.devices].sort(
      (a, b) => Number(b.online) - Number(a.online) || new Date(b.lastSeen).getTime() - new Date(a.lastSeen).getTime(),
    ),
  );

  return { devices, stats, overview, error, loading, loaded, refresh, daily, frames30, onlineHours30, hasTraffic, recent, days };
}
