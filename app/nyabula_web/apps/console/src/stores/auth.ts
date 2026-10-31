/* Auth store: session token + current user. */
import { defineStore } from 'pinia';
import { ref } from 'vue';
import * as api from '../api/client';
import { useDevicesStore } from './devices';
import { useRemoteStore } from './remote';
import { useStatsStore } from './stats';
import { useEventsStore } from './events';

export const useAuthStore = defineStore('auth', () => {
  const user = ref<api.User | null>(null);
  const ready = ref(false); // initial session probe finished
  const busy = ref(false);
  const lastError = ref<string | null>(null);
  function clearWorkspace() {
    useRemoteStore().disconnect();
    useDevicesStore().reset();
    useStatsStore().reset();
    useEventsStore().reset();
  }

  /** Restore session from stored token on app start. */
  async function init(): Promise<void> {
    if (api.getToken()) {
      try {
        user.value = (await api.authMe()).user;
        useEventsStore().add('已恢复登录会话', 'ok');
      } catch {
        api.setToken(null);
        user.value = null;
      }
    }
    ready.value = true;
  }

  async function run(fn: () => Promise<{ token: string; user: api.User }>): Promise<boolean> {
    busy.value = true;
    lastError.value = null;
    try {
      const res = await fn();
      clearWorkspace();
      api.setToken(res.token);
      user.value = res.user;
      useEventsStore().add('登录成功', 'ok');
      return true;
    } catch (e) {
      lastError.value = e instanceof Error ? e.message : String(e);
      return false;
    } finally {
      busy.value = false;
    }
  }

  const login = (email: string, password: string) => run(() => api.authLogin(email, password));
  const register = (email: string, password: string, name: string) =>
    run(() => api.authRegister(email, password, name));

  async function logout(): Promise<boolean> {
    if (busy.value) return false;
    busy.value = true;
    lastError.value = null;
    try {
      await api.authLogout();
    } catch (e) {
      if (!(e instanceof api.ApiError && e.status === 401)) {
        lastError.value = e instanceof Error ? e.message : String(e);
        return false;
      }
    } finally {
      busy.value = false;
    }
    clearWorkspace();
    api.setToken(null);
    user.value = null;
    return true;
  }

  return { user, ready, busy, lastError, init, login, register, logout };
});
