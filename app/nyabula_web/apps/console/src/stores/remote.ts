/* Remote-panel store: NyaLink over the Cloud relay for one device at a time.
 * Semantics identical to a LAN connection (cloud.md §3): after the ws
 * upgrade the stream is pure NyaLink — sys.hello with a device-level token,
 * sys.pair with the on-eye code when required.
 *
 * The device-level NyaLink token is stored per deviceId (not per URL: the
 * relay URL embeds the rotating session token). */
import { defineStore } from 'pinia';
import { computed, ref, shallowRef } from 'vue';
import { NyaLinkClient, NyaLinkError, type ConnState, type DeviceInfo } from '@nyabula/nyalink';
import { coreEyeState, isCoreEyeSnapshot, type EyeState } from '@nyabula/eye-engine';
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
  const passwordRequired = ref(false);
  const passwordSet = ref(true);
  let probing: Promise<void> | null = null;
  const canControl = computed(() => state.value === 'connected' && (role.value === 'owner' || role.value === 'family'));

  function connect(id: string): void {
    disconnect();
    deviceId.value = id;
    lastError.value = null;
    lastEyeState.value = null;
    const c = new NyaLinkClient({
      clientKind: 'console',
      version: '0.1.0',
      manualHello: true,
      loadToken: () => localStorage.getItem(DEV_TOKEN_PREFIX + id),
      saveToken: (_u, token) => localStorage.setItem(DEV_TOKEN_PREFIX + id, token),
    });
    c.onStateChange((s) => {
      if (client.value !== c) return;
      state.value = s;
      if (s === 'authenticating' && c.awaitingAuth) {
        probing = probeAuth(c, id);
      }
      if (s === 'closed' && c.authError) {
        localStorage.removeItem(DEV_TOKEN_PREFIX + id);
        passwordRequired.value = true;
        lastError.value = '设备登录已失效，请重新输入设备密码';
      }
      if (s === 'connected') {
        device.value = c.device;
        role.value = c.role;
        passwordRequired.value = false;
      }
    });
    c.on('eye.state', (data) => {
      if (client.value !== c) return;
      if (data.schema === 'nyabula.eye.v1') {
        if (isCoreEyeSnapshot(data)) lastEyeState.value = coreEyeState(data);
      } else {
        lastEyeState.value = data as EyeState;
      }
    });
    client.value = c;
    c.connect(relayUrl(id));
  }

  async function probeAuth(c: NyaLinkClient, id: string): Promise<void> {
    try {
      const auth = await c.request('sys.auth.state', {});
      if (client.value !== c) return;
      passwordSet.value = auth.passwordSet === true;
      const token = localStorage.getItem(DEV_TOKEN_PREFIX + id);
      if (token) await c.authenticate(token);
      else passwordRequired.value = true;
    } catch (e) {
      if (client.value !== c) return;
      if (e instanceof NyaLinkError && ['ENOTFOUND', 'EAUTH'].includes(e.code)) {
        try { await c.authenticate(localStorage.getItem(DEV_TOKEN_PREFIX + id)); }
        catch (error) { lastError.value = error instanceof Error ? error.message : String(error); }
      } else {
        lastError.value = e instanceof Error ? e.message : String(e);
      }
    }
  }

  async function login(password: string): Promise<boolean> {
    const id = deviceId.value;
    if (!id) return false;
    lastError.value = null;
    try {
      if (!client.value?.awaitingAuth) {
        localStorage.removeItem(DEV_TOKEN_PREFIX + id);
        connect(id);
        const end = Date.now() + 10000;
        while (!client.value?.awaitingAuth && Date.now() < end) {
          await new Promise(resolve => setTimeout(resolve, 50));
          if (deviceId.value !== id) throw new Error('设备已切换');
        }
        await probing;
      }
      const c = client.value;
      if (!c?.awaitingAuth) throw new Error('设备未连接，请重试');
      const result = await c.request('sys.login', { password });
      if (typeof result.token !== 'string' || !result.token) throw new Error('设备未返回登录凭据');
      if (client.value !== c) return false;
      await c.authenticate(result.token);
      localStorage.setItem(DEV_TOKEN_PREFIX + id, result.token);
      return true;
    } catch (e) {
      lastError.value = e instanceof Error ? e.message : String(e);
      return false;
    }
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
    passwordRequired.value = false;
    probing = null;
    lastEyeState.value = null;
  }

  async function send(topic: string, data: Record<string, unknown>): Promise<boolean> {
    const c = client.value;
    if (!c || !canControl.value) return false;
    lastError.value = null;
    try {
      const native = c.capabilities.includes('eyes.native-v1');
      if (native && topic === 'eye.mode') {
        await c.request('eyes.expression', { expression: data.mode });
      } else if (native && topic === 'eye.look') {
        await c.request('eyes.gaze', { x: data.x ?? 0, y: data.y ?? 0, hold_ms: data.release ? 0 : 2200 });
      } else {
        await c.request(topic, data);
      }
      return true;
    } catch (e) {
      lastError.value = e instanceof Error ? e.message : String(e);
      return false;
    }
  }

  const clockOffsetMs = () => client.value?.clockOffsetMs ?? 0;

  return {
    client,
    passwordRequired,
    passwordSet,
    login,
    deviceId,
    state,
    canControl,
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
