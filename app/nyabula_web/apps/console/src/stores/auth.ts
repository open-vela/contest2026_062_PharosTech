/* Auth store: session token + current user. */
import { defineStore } from 'pinia';
import { ref } from 'vue';
import * as api from '../api/client';

export const useAuthStore = defineStore('auth', () => {
  const user = ref<api.User | null>(null);
  const ready = ref(false); // initial session probe finished
  const busy = ref(false);
  const lastError = ref<string | null>(null);

  /** Restore session from stored token on app start. */
  async function init(): Promise<void> {
    if (api.getToken()) {
      try {
        user.value = (await api.authMe()).user;
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
      api.setToken(res.token);
      user.value = res.user;
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

  function logout(): void {
    api.setToken(null);
    user.value = null;
  }

  return { user, ready, busy, lastError, init, login, register, logout };
});
