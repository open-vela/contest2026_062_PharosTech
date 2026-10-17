/* Remote-panel store: NyaLink over the Cloud relay for one device at a time.
 * Semantics identical to a LAN connection (cloud.md §3): after the ws
 * upgrade the stream is pure NyaLink — sys.hello with a device-level token,
 * sys.pair with the on-eye code when required.
 *
 * The device-level NyaLink token is stored per deviceId (not per URL: the
 * relay URL embeds the rotating session token). */
import { defineStore } from 'pinia';
import { ref, shallowRef } from 'vue';
import { NyaLinkClient, type ConnState, type DeviceInfo } from '@nyabula/nyalink';
import type { EyeState } from '@nyabula/eye-engine';
import { relayUrl } from '../api/client';

const DEV_TOKEN_PREFIX = 'nyacloud.devtoken:';

export const useRemoteStore = defineStore('remote', () => {
  const client = shallowRef<NyaLinkClient | null>(null);
  const deviceId = ref<string | null>(null);
  const state = ref<ConnState>('idle');
  const device = ref<DeviceInfo | null>(null);
  const role = ref<string | null>(null);
  const lastError = ref<string | null>(null);
  const lastEyeState = shallowRef<EyeState | null>(null);

  function connect(id: string): void {
    disconnect();
    deviceId.value = id;
    lastError.value = null;
    lastEyeState.value = null;
    const c = new NyaLinkClient({
      clientKind: 'console',
      version: '0.1.0',
      loadToken: () => localStorage.getItem(DEV_TOKEN_PREFIX + id),
      saveToken: (_u, token) => localStorage.setItem(DEV_TOKEN_PREFIX + id, token),
    });
    c.onStateChange((s) => {
      state.value = s;
      if (s === 'connected') {
        device.value = c.device;
        role.value = c.role;
      }
    });
    c.on('eye.state', (data) => {
      lastEyeState.value = data as EyeState;
    });
    client.value = c;
    c.connect(relayUrl(id));
  }

  async function pair(code: string, name: string): Promise<void> {
    lastError.value = null;
    try {
      await client.value?.pair(code, name);
    } catch (e) {
      lastError.value = e instanceof Error ? e.message : String(e);
      throw e;
    }
  }

  function disconnect(): void {
    client.value?.close();
    client.value = null;
    deviceId.value = null;
    device.value = null;
    role.value = null;
    state.value = 'idle';
    lastEyeState.value = null;
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
    deviceId,
    state,
    device,
    role,
    lastError,
    lastEyeState,
    connect,
    pair,
    disconnect,
    send,
    clockOffsetMs,
  };
});
