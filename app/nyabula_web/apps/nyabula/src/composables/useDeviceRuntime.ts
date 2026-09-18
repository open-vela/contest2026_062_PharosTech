import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { defineStore } from 'pinia';
import { useSessionStore } from '../stores/session';

export interface DeviceInterface { name: string; index: number; up?: boolean; loopback?: boolean; ipv4?: string; ssid?: string }
export interface DeviceRuntime {
  os: string; arch: string; simulator: boolean; sampledAtMs: number; uptimeMs: number;
  memory: { totalBytes: number; usedBytes: number; freeBytes: number };
  cpu: { available: boolean; percent?: number; source?: string; samples?: number; reason?: string };
  storage: { path: string; available: boolean; totalBytes?: number; freeBytes?: number; error?: number }[];
  network: { available: boolean; interfaces: DeviceInterface[]; error?: number; truncated?: boolean };
  audioDevices: { path: string; available: boolean; input?: boolean; output?: boolean; formats?: number; error?: number }[];
}

const useDeviceRuntimeStore = defineStore('device-runtime', () => {
  const session = useSessionStore();
  const snapshot = ref<DeviceRuntime | null>(null);
  const busy = ref(false);
  const error = ref('');
  const available = computed(() => session.connected && session.isOwner && session.client?.capabilities.includes('core.device-v1') === true);
  let generation = 0;
  watch(() => session.client, () => { generation++; snapshot.value = null; busy.value = false; error.value = ''; });
  async function refresh(): Promise<void> {
    if (!available.value || busy.value) return;
    const current = generation;
    busy.value = true;
    try {
      const data = await session.request('device.status', {});
      if (current !== generation) return;
      snapshot.value = data as unknown as DeviceRuntime;
      error.value = '';
    } catch (cause) { if (current === generation) error.value = String(cause); }
    finally { if (current === generation) busy.value = false; }
  }
  return { snapshot, busy, error, available, refresh };
});

export function formatDeviceBytes(bytes: number | undefined): string {
  if (bytes === undefined || !Number.isFinite(bytes) || bytes < 0) return '—';
  if (bytes >= 1024 ** 3) return `${(bytes / 1024 ** 3).toFixed(2)} GiB`;
  if (bytes >= 1024 ** 2) return `${(bytes / 1024 ** 2).toFixed(1)} MiB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${bytes} B`;
}

export function useDeviceRuntime() {
  const device = useDeviceRuntimeStore();
  const timer = setInterval(() => { void device.refresh(); }, 2500);
  watch(() => device.available, ready => { if (ready) void device.refresh(); }, { immediate: true });
  onBeforeUnmount(() => clearInterval(timer));
  return device;
}
