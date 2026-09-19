/* Pinia store owning the NyaLink client connection. */
import { defineStore } from 'pinia';
import { ref, shallowRef } from 'vue';
import { NyaLinkClient, type ConnState, type DeviceInfo } from '@nyabula/nyalink';
import type { EyeState } from '@nyabula/eye-engine';

const TOKEN_PREFIX = 'nyalink.token:';

export const useLinkStore = defineStore('link', () => {
  const client = shallowRef<NyaLinkClient | null>(null);
  const state = ref<ConnState>('idle');
  const device = ref<DeviceInfo | null>(null);
  const role = ref<string | null>(null);
  const lastError = ref<string | null>(null);
  const url = ref(localStorage.getItem('nyalink.url') ?? 'ws://localhost:7788/nyalink');
  /** Latest eye.state event, forwarded to the EyeEngine by EyeCanvas. */
  const lastEyeState = shallowRef<EyeState | null>(null);

  function ensureClient(): NyaLinkClient {
    if (client.value) return client.value;
    const c = new NyaLinkClient({
      clientKind: 'web',
      version: '0.1.0',
      loadToken: (u) => localStorage.getItem(TOKEN_PREFIX + u),
      saveToken: (u, token) => localStorage.setItem(TOKEN_PREFIX + u, token),
    });
    c.onStateChange((s) => {
      state.value = s;
      if (s === 'connected') {
        device.value = c.device;
        role.value = c.role as never;
      }
    });
    c.on('eye.state', (data) => {
      lastEyeState.value = data as EyeState;
    });
    client.value = c;
    return c;
  }

  function connect(target?: string): void {
    lastError.value = null;
    if (target) url.value = target;
    localStorage.setItem('nyalink.url', url.value);
    ensureClient().connect(url.value);
  }

  async function pair(code: string, name: string): Promise<void> {
    lastError.value = null;
    try {
      await ensureClient().pair(code, name);
    } catch (e) {
      lastError.value = e instanceof Error ? e.message : String(e);
      throw e;
    }
  }

  function disconnect(): void {
    client.value?.close();
  }

  /** Disconnect and drop the persisted pairing token for the current URL. */
  function forget(): void {
    localStorage.removeItem(TOKEN_PREFIX + url.value);
    disconnect();
  }

  /** Request that surfaces errors to the caller (unlike fire-and-forget send). */
  function request(topic: string, data: Record<string, unknown> = {}): Promise<Record<string, unknown>> {
    const c = client.value;
    if (!c || state.value !== 'connected') {
      return Promise.reject(new Error('not connected'));
    }
    return c.request(topic, data);
  }

  /** Subscribe to a NyaLink event topic. Returns an unsubscribe fn. */
  function onEvent(topic: string, cb: (data: Record<string, unknown>) => void): () => void {
    return ensureClient().on(topic, cb);
  }

  async function send(topic: string, data: Record<string, unknown>): Promise<void> {
    const c = client.value;
    if (!c || state.value !== 'connected') return;
    try {
      await c.request(topic, data);
    } catch (e) {
      lastError.value = e instanceof Error ? e.message : String(e);
    }
  }

  const clockOffsetMs = () => client.value?.clockOffsetMs ?? 0;

  return {
    client,
    state,
    device,
    role,
    lastError,
    url,
    lastEyeState,
    connect,
    pair,
    disconnect,
    forget,
    send,
    request,
    onEvent,
    clockOffsetMs,
  };
});
