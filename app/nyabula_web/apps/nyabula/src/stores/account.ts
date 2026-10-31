/* Cloud account domain (HTTP). */
import { defineStore } from 'pinia';
import { computed, ref } from 'vue';
import { cloudApi, getCloudToken, setCloudToken, type CloudDevice, type Overview, type User } from '../api/cloud';

export const useAccountStore = defineStore('account', () => {
  const user = ref<User | null>(null);
  const devices = ref<CloudDevice[]>([]);
  const overview = ref<Overview | null>(null);
  const loading = ref(false);
  const checked = ref(false);

  const loggedIn = computed(() => !!user.value);

  async function restore(): Promise<void> {
    if (checked.value) return;
    checked.value = true;
    if (!getCloudToken()) return;
    try {
      user.value = (await cloudApi.me()).user;
    } catch {
      setCloudToken(null);
      user.value = null;
    }
  }

  async function login(email: string, password: string): Promise<void> {
    const r = await cloudApi.login(email, password);
    setCloudToken(r.token);
    user.value = r.user;
  }
  async function register(email: string, password: string, name: string): Promise<void> {
    const r = await cloudApi.register(email, password, name);
    setCloudToken(r.token);
    user.value = r.user;
  }
  function logout(): void {
    setCloudToken(null);
    user.value = null;
    devices.value = [];
    overview.value = null;
  }

  async function refreshDevices(): Promise<void> {
    loading.value = true;
    try {
      devices.value = (await cloudApi.listDevices()).devices;
    } finally {
      loading.value = false;
    }
  }
  async function refreshOverview(): Promise<void> {
    overview.value = await cloudApi.overview();
  }
  async function claim(deviceId: string, claimCode: string): Promise<CloudDevice> {
    const r = await cloudApi.claimDevice(deviceId, claimCode);
    await refreshDevices();
    return r.device;
  }
  async function unclaim(deviceId: string): Promise<void> {
    await cloudApi.unclaimDevice(deviceId);
    await refreshDevices();
  }

  return { user, devices, overview, loading, loggedIn, restore, login, register, logout, refreshDevices, refreshOverview, claim, unclaim };
});
