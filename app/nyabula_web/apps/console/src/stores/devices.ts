/* Device list store: list / claim / unclaim. */
import { defineStore } from 'pinia';
import { ref } from 'vue';
import * as api from '../api/client';
import { useEventsStore } from './events';

export const useDevicesStore = defineStore('devices', () => {
  const devices = ref<api.Device[]>([]);
  const loading = ref(false);
  const lastError = ref<string | null>(null);
  let generation = 0;
  function reset() {
    generation++;
    devices.value = [];
    lastError.value = null;
    loading.value = false;
  }

  async function refresh(): Promise<void> {
    const current = generation;
    loading.value = true;
    lastError.value = null;
    try {
      const next = (await api.listDevices()).devices;
      if (current !== generation) return;
      for (const d of next) {
        const previous = devices.value.find(p => p.deviceId === d.deviceId);
        if (previous && previous.online !== d.online) {
          useEventsStore().add(`${d.name || d.deviceId} ${d.online ? '已上线' : '已离线'}`, d.online ? 'ok' : 'warn');
        }
      }
      devices.value = next;
    } catch (e) {
      if (current !== generation) return;
      lastError.value = e instanceof Error ? e.message : String(e);
    } finally {
      if (current === generation) loading.value = false;
    }
  }

  async function claim(deviceId: string, claimCode: string): Promise<boolean> {
    lastError.value = null;
    try {
      await api.claimDevice(deviceId, claimCode);
      useEventsStore().add(`已认领设备 ${deviceId}`, 'ok');
      await refresh();
      return true;
    } catch (e) {
      lastError.value = e instanceof Error ? e.message : String(e);
      return false;
    }
  }

  async function unclaim(deviceId: string): Promise<boolean> {
    lastError.value = null;
    try {
      await api.unclaimDevice(deviceId);
      useEventsStore().add(`已解绑设备 ${deviceId}`);
      devices.value = devices.value.filter((d) => d.deviceId !== deviceId);
      return true;
    } catch (e) {
      lastError.value = e instanceof Error ? e.message : String(e);
      return false;
    }
  }

  return { devices, loading, lastError, refresh, claim, unclaim, reset };
});
