/* Device list store: list / claim / unclaim. */
import { defineStore } from 'pinia';
import { ref } from 'vue';
import * as api from '../api/client';

export const useDevicesStore = defineStore('devices', () => {
  const devices = ref<api.Device[]>([]);
  const loading = ref(false);
  const lastError = ref<string | null>(null);

  async function refresh(): Promise<void> {
    loading.value = true;
    lastError.value = null;
    try {
      devices.value = (await api.listDevices()).devices;
    } catch (e) {
      lastError.value = e instanceof Error ? e.message : String(e);
    } finally {
      loading.value = false;
    }
  }

  async function claim(deviceId: string, claimCode: string): Promise<boolean> {
    lastError.value = null;
    try {
      await api.claimDevice(deviceId, claimCode);
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
      devices.value = devices.value.filter((d) => d.deviceId !== deviceId);
      return true;
    } catch (e) {
      lastError.value = e instanceof Error ? e.message : String(e);
      return false;
    }
  }

  return { devices, loading, lastError, refresh, claim, unclaim };
});
