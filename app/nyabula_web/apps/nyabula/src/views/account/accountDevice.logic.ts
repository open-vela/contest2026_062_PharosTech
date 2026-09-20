/* Per-device statistics page logic (GET /devices/:id/stats). */
import { computed } from 'vue';
import { useRouter } from 'vue-router';
import { cloudApi, type DailyStat } from '../../api/cloud';
import { useAccountStore } from '../../stores/account';
import { cloudKey } from '../../stores/session';
import { useAsyncTask } from '../../composables/useRequest';

export function formatDuration(seconds: number): string {
  const s = Math.max(0, Math.round(seconds));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (h === 0 && m === 0) return s ? `${s} 秒` : '0';
  return h ? `${h} 时 ${m} 分` : `${m} 分`;
}

export function useAccountDevicePage(id: string) {
  const account = useAccountStore();
  const router = useRouter();

  const loader = useAsyncTask(() => cloudApi.deviceStats(id), { errorPrefix: '加载设备统计失败', holdRoute: true, immediate: true });
  const stats = computed(() => loader.data.value);
  const device = computed(() => account.devices.find((d) => d.deviceId === id) ?? null);
  const title = computed(() => device.value?.name || id);

  /* Oldest first so the chart reads left to right. */
  const daily = computed<DailyStat[]>(() => [...(stats.value?.daily ?? [])].sort((a, b) => a.date.localeCompare(b.date)));
  const totals = computed(() => ({
    onlineSeconds: daily.value.reduce((s, d) => s + d.onlineSeconds, 0),
    framesUp: daily.value.reduce((s, d) => s + d.framesUp, 0),
    framesDown: daily.value.reduce((s, d) => s + d.framesDown, 0),
  }));

  function openRemote(): void {
    void router.push({ name: 'home', params: { key: cloudKey(id) } });
  }
  function back(): void {
    void router.push({ name: 'account' });
  }

  return { id, account, loader, stats, device, title, daily, totals, openRemote, back, reload: loader.run };
}
