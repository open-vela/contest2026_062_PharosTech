/* Device detail: daily stats (GET /devices/{id}/stats) + the remote panel
 * over the Cloud relay (pure NyaLink after the ws upgrade: hello / pair /
 * eye.look / eye.mode, cloud.md §3). The relay connects while a detail
 * view for `id` is mounted and disconnects when it goes away or switches
 * device. Shared by every skin. */
import { computed, onBeforeUnmount, ref, watch, type Ref } from 'vue';
import { MODES } from '@nyabula/eye-engine';
import { useRemoteStore } from '../stores/remote';
import { useStatsStore } from '../stores/stats';
import { useDevicesStore } from '../stores/devices';
import { dayLabel } from './format';

export const MODE_LABELS: Record<string, string> = {
  idle: '待机', curious: '好奇', happy: '开心', processing: '处理中',
  star: '星星眼', heart: '爱心眼', sleepy: '困倦', sleep: '睡眠',
  angry: '生气', sad: '委屈', surprise: '惊讶', dizzy: '晕', derp: '呆',
};

export function useDeviceDetail(id: Readonly<Ref<string | null>>, days = 30) {
  const remote = useRemoteStore();
  const stats = useStatsStore();
  const devices = useDevicesStore();

  const statsError = computed(() => (id.value ? stats.errors[id.value] ?? null : null));
  const current = computed(() => (id.value ? stats.byId[id.value] ?? null : null));
  const series = computed(() => (id.value ? stats.series(id.value, days) : []));
  const device = computed(() => devices.devices.find((d) => d.deviceId === id.value) ?? null);
  const title = computed(() => remote.device?.name ?? device.value?.name ?? id.value ?? '');

  watch(
    id,
    (v, old) => {
      if (old && old !== v) remote.disconnect();
      if (!v) return;
      remote.connect(v);
      void stats.load(v);
    },
    { immediate: true },
  );
  onBeforeUnmount(() => remote.disconnect());

  /* ---- stats (oldest first) ---- */
  const onlineBars = computed(() => series.value.map((d) => ({ label: dayLabel(d.date), value: d.onlineSeconds })));
  const frameBars = computed(() => series.value.map((d) => ({ label: dayLabel(d.date), value: d.framesUp + d.framesDown })));
  const rate = computed(() => (id.value ? stats.onlineRate(id.value, days) : null));
  const frames30 = computed(() => series.value.reduce((n, d) => n + d.framesUp + d.framesDown, 0));

  /* ---- remote panel ----
   * Chip selection derives from the latest eye.state so every client stays
   * in sync; a click sets a pending (optimistic) value that the next remote
   * state supersedes. */
  const pendingMode = ref<string | null>(null);
  const activeMode = computed(() => pendingMode.value ?? remote.lastEyeState?.expression?.mode ?? 'idle');
  watch(
    () => remote.lastEyeState,
    () => {
      pendingMode.value = null;
    },
  );
  const pairing = ref(false);
  const pairError = ref<string | null>(null);
  const connected = computed(() => remote.state === 'connected');
  const needPair = computed(() => remote.state === 'pairing-required');
  const showPairOverlay = computed(() => needPair.value || (pairing.value && remote.state === 'authenticating'));

  const pillLabel = computed(() => {
    switch (remote.state) {
      case 'connected':
        return remote.device?.name ? `已接管 · ${remote.device.name}` : '已接管';
      case 'connecting':
        return '连接中…';
      case 'authenticating':
        return '鉴权中…';
      case 'pairing-required':
        return '待配对';
      case 'reconnecting':
        return '重连中…';
      case 'closed':
        return '已断开';
      default:
        return '未连接';
    }
  });
  const pillTone = computed<'ok' | 'busy' | 'off'>(() =>
    remote.state === 'connected' ? 'ok' : ['connecting', 'authenticating', 'reconnecting'].includes(remote.state) ? 'busy' : 'off',
  );

  function onLook(data: { x?: number; y?: number; release?: boolean }): void {
    if (!remote.canControl) return;
    void remote.send('eye.look', data);
  }
  async function pickMode(mode: string): Promise<void> {
    if (!remote.canControl) return;
    pendingMode.value = mode;
    if (!(await remote.send('eye.mode', { mode }))) pendingMode.value = null;
  }
  async function doPair(code: string): Promise<void> {
    pairing.value = true;
    pairError.value = null;
    try {
      await remote.pair(code, 'Nyabula 控制台');
    } catch {
      pairError.value =
        remote.lastError && /wrong pairing code|EPERM/i.test(remote.lastError) ? '配对码错误' : (remote.lastError ?? '配对失败');
    } finally {
      pairing.value = false;
    }
  }
  function reconnect(): void {
    if (id.value) remote.connect(id.value);
  }

  return {
    remote, stats, device, title, current, series, statsError, onlineBars, frameBars, rate, frames30,
    modes: MODES as readonly string[], MODE_LABELS, activeMode, pickMode, onLook,
    pairing, pairError, doPair, connected, needPair, showPairOverlay, pillLabel, pillTone, reconnect,
  };
}
