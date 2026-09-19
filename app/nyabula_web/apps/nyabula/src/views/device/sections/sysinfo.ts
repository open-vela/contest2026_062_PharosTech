/* sys.info payload shape + formatters shared by overview / network sections. */
import { computed } from 'vue';
import { useSessionStore } from '../../../stores/session';
import { useAsyncTask } from '../../../composables/useRequest';

export interface SysInfo {
  device?: { id?: string; name?: string; coreVersion?: string };
  uptime?: number;
  battery?: number | { level?: number; charging?: boolean };
  wifi?: { ssid?: string; rssi?: number } | string;
  /** `external`: mains powered, there is no battery to report. */
  power?: { source?: string };
}

export function fmtUptime(s: number | undefined): string {
  if (typeof s !== 'number') return '—';
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (d > 0) return `${d} 天 ${h} 小时`;
  return h > 0 ? `${h} 小时 ${m} 分` : `${m} 分`;
}

export interface BatteryState {
  level: number | null;
  charging: boolean;
  /** Mains powered with no charge level: show "plugged in", not a dash. */
  external: boolean;
}

export function batteryOf(info: SysInfo | null): BatteryState {
  const b = info?.battery;
  let level: number | null = null;
  let charging = false;
  if (typeof b === 'number') level = b;
  else if (b && typeof b === 'object') {
    level = typeof b.level === 'number' ? b.level : null;
    charging = b.charging === true;
  }
  return { level, charging, external: level === null && info?.power?.source === 'external' };
}

export function wifiOf(info: SysInfo | null): { ssid: string | null; rssi: number | null } {
  const w = info?.wifi;
  if (!w) return { ssid: null, rssi: null };
  if (typeof w === 'string') return { ssid: w, rssi: null };
  return { ssid: w.ssid ?? null, rssi: typeof w.rssi === 'number' ? w.rssi : null };
}

/** 0-4 signal bars from RSSI (dBm). */
export function rssiBars(rssi: number | null): number {
  if (rssi === null) return 0;
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  return 1;
}

export function useSysInfo() {
  const session = useSessionStore();
  const task = useAsyncTask<SysInfo>(() => session.request('sys.info') as Promise<SysInfo>, { errorPrefix: '读取设备信息失败', holdRoute: true, immediate: true });
  const info = computed(() => task.data.value);
  return { session, task, info };
}
