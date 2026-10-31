/* Home dashboard: device at a glance, live eye preview, quick actions,
 * attention items, widgets and a lightweight activity feed. */
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { useRouter } from 'vue-router';
import { useSessionStore } from '../../stores/session';
import { MODE_LABELS, SCENE_META, useEyeStore } from '../../stores/eye';
import { usePluginsStore } from '../../stores/plugins';
import { useAccountStore } from '../../stores/account';
import { useAsyncTask } from '../../composables/useRequest';
import { batteryOf, fmtUptime, wifiOf, rssiBars, type SysInfo } from '../device/sections/sysinfo';

export interface ActivityItem {
  id: number;
  at: number;
  icon: string;
  text: string;
  tone?: 'ok' | 'warn' | 'info';
}

export interface QuickAction {
  id: string;
  label: string;
  icon: string;
  hint?: string;
  run: () => void;
}

const QUICK_MODES = ['idle', 'happy', 'curious', 'sleepy', 'heart', 'star'];

export function useHomePage() {
  const session = useSessionStore();
  const eye = useEyeStore();
  const plugins = usePluginsStore();
  const account = useAccountStore();
  const router = useRouter();

  const key = computed(() => session.deviceKey ?? '');
  function go(name: string, params: Record<string, string> = {}): void {
    void router.push({ name, params: { key: key.value, ...params } });
  }

  /* ---- sys.info + cloud.status ---- */
  const info = ref<SysInfo | null>(null);
  const cloud = ref<{ enabled?: boolean; connected?: boolean; url?: string } | null>(null);
  const infoTask = useAsyncTask(async () => {
    const [i, c] = await Promise.allSettled([session.request('sys.info'), session.request('cloud.status')]);
    if (i.status === 'fulfilled') info.value = i.value as SysInfo;
    if (c.status === 'fulfilled') cloud.value = c.value as typeof cloud.value;
    return true;
  }, { holdRoute: true });
  watch(
    () => session.connected,
    (c) => {
      if (c) void infoTask.run();
      else {
        info.value = null;
        cloud.value = null;
      }
    },
    { immediate: true },
  );

  const battery = computed(() => batteryOf(info.value));
  const wifi = computed(() => wifiOf(info.value));
  const bars = computed(() => rssiBars(wifi.value.rssi));
  const uptime = computed(() => fmtUptime(info.value?.uptime));
  const deviceName = computed(() => session.device?.name ?? info.value?.device?.name ?? '未命名设备');
  const coreVersion = computed(() => session.device?.coreVersion ?? info.value?.device?.coreVersion ?? null);

  const stateLabel = computed(() => {
    const labels: Record<string, string> = {
      idle: '未连接', connecting: '连接中', authenticating: '认证中', 'pairing-required': '等待配对',
      connected: '在线', reconnecting: '重连中', closed: '已断开',
    };
    return labels[session.state] ?? session.state;
  });
  const stateTone = computed<'ok' | 'warn' | 'err'>(() =>
    session.state === 'connected' ? 'ok' : session.state === 'closed' || session.state === 'idle' ? 'err' : 'warn',
  );

  /* ---- eye summary ---- */
  const modeLabel = computed(() => MODE_LABELS[eye.activeMode] ?? eye.activeMode);
  const sceneLabel = computed(() => (eye.activeScene ? SCENE_META[eye.activeScene]?.label ?? eye.activeScene : null));
  const quickModes = QUICK_MODES.map((m) => ({ id: m, label: MODE_LABELS[m] ?? m }));

  /* ---- plugins summary ---- */
  const pluginCount = computed(() => plugins.list.length);
  const runningCount = computed(() => plugins.list.filter((p) => (p.state ?? 'running') === 'running').length);
  const missingPerms = computed(() =>
    plugins.list
      .map((p) => ({ plugin: p, missing: (p.permissions ?? []).filter((x) => !x.granted) }))
      .filter((x) => x.missing.length > 0),
  );

  /* ---- attention items ---- */
  const attention = computed(() => {
    const items: { id: string; icon: string; title: string; sub: string; tone: 'warn' | 'info' | 'err'; run: () => void }[] = [];
    if (session.state === 'pairing-required') {
      items.push({ id: 'pair', icon: 'key', title: '设备等待配对', sub: '输入设备屏幕上的 6 位配对码', tone: 'warn', run: () => go('eye') });
    }
    if (session.connected && cloud.value && cloud.value.enabled && !cloud.value.connected) {
      items.push({ id: 'cloud', icon: 'cloud_off', title: '云中继未连通', sub: cloud.value.url ?? '检查 Cloud 地址与网络', tone: 'warn', run: () => go('device', { section: 'cloud' }) });
    }
    if (session.connected && cloud.value && !cloud.value.enabled) {
      items.push({ id: 'cloud-off', icon: 'cloud', title: '未启用云中继', sub: '开启后可在外网远程访问', tone: 'info', run: () => go('device', { section: 'cloud' }) });
    }
    for (const m of missingPerms.value.slice(0, 3)) {
      items.push({
        id: `perm:${m.plugin.id}`,
        icon: 'shield',
        title: `${m.plugin.name} 缺少 ${m.missing.length} 项权限`,
        sub: m.missing.map((x) => x.name).join('、'),
        tone: 'info',
        run: () => go('plugin-permissions', { id: m.plugin.id }),
      });
    }
    if (battery.value.level !== null && battery.value.level <= 20 && !battery.value.charging) {
      items.push({ id: 'battery', icon: 'battery_low', title: '电量偏低', sub: `${battery.value.level}%，请及时充电`, tone: 'err', run: () => go('device') });
    }
    return items;
  });

  /* ---- quick actions ---- */
  const quickActions = computed<QuickAction[]>(() => [
    { id: 'eye', label: '眼睛控制', icon: 'visibility', hint: '表情 · 场景 · 注视', run: () => go('eye') },
    { id: 'agent', label: '对话', icon: 'chat', hint: '和 Nyabula 聊聊', run: () => go('agent') },
    { id: 'services', label: '功能', icon: 'widgets', hint: '音乐 · 闹钟 · 天气…', run: () => go('services') },
    { id: 'plugins', label: '插件', icon: 'extension', hint: `${pluginCount.value} 个已安装`, run: () => go('plugins') },
    { id: 'device', label: '设备', icon: 'devices', hint: '网络 · 云 · 权限', run: () => go('device') },
    { id: 'sleep', label: eye.activeMode === 'sleep' ? '唤醒' : '休眠', icon: 'moon', hint: '切换睡眠表情', run: () => void eye.setMode(eye.activeMode === 'sleep' ? 'idle' : 'sleep') },
  ]);

  /* ---- activity feed (client-side, from live events) ---- */
  const activity = ref<ActivityItem[]>([]);
  let seq = 0;
  function push(icon: string, text: string, tone?: ActivityItem['tone']): void {
    activity.value = [{ id: ++seq, at: Date.now(), icon, text, tone }, ...activity.value].slice(0, 12);
  }
  let lastMode: string | null = null;
  let lastScene: string | null | undefined;
  const stopEye = watch(
    () => eye.lastState,
    (s) => {
      if (!s) return;
      const mode = s.expression?.mode ?? null;
      const scene = s.scene?.type ?? null;
      if (lastMode !== null && mode !== lastMode) push('face', `表情切换为「${MODE_LABELS[mode ?? ''] ?? mode}」`);
      if (lastScene !== undefined && scene !== lastScene) {
        push('widgets', scene ? `进入场景「${SCENE_META[scene]?.label ?? scene}」` : '退出场景');
      }
      lastMode = mode;
      lastScene = scene;
    },
  );
  const stopConn = watch(
    () => session.state,
    (s, prev) => {
      if (prev === undefined) return;
      if (s === 'connected') push('link', '设备已连接', 'ok');
      else if (s === 'reconnecting') push('sync', '连接丢失，重连中', 'warn');
      else if (s === 'closed') push('link_off', '连接已断开', 'warn');
    },
  );
  let unsubPatch: (() => void) | null = null;
  watch(
    () => session.client,
    (c) => {
      unsubPatch?.();
      unsubPatch = c ? c.on('ui.patch', (d) => push('extension', `插件 ${String(d.pluginId ?? '')} 更新了界面`)) : null;
    },
    { immediate: true },
  );
  onBeforeUnmount(() => {
    stopEye();
    stopConn();
    unsubPatch?.();
  });

  function ago(t: number): string {
    const s = Math.round((Date.now() - t) / 1000);
    if (s < 5) return '刚刚';
    if (s < 60) return `${s} 秒前`;
    const m = Math.round(s / 60);
    return m < 60 ? `${m} 分钟前` : `${Math.round(m / 60)} 小时前`;
  }

  return {
    session, eye, plugins, account, go,
    info, cloud, infoTask, battery, wifi, bars, uptime, deviceName, coreVersion, stateLabel, stateTone,
    modeLabel, sceneLabel, quickModes,
    pluginCount, runningCount, missingPerms, attention, quickActions, activity, ago,
  };
}
