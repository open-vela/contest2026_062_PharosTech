/* Device session: one NyaLink connection at a time, addressed by a deviceKey.
 *   lan:<host>:<port>   direct LAN WebSocket
 *   cloud:<deviceId>    Cloud relay (identical NyaLink semantics)
 * Pages never branch on the transport; only the URL builder does.
 * Also keeps the "known devices" list for the connect screen. */
import { defineStore } from 'pinia';
import { computed, ref, shallowRef } from 'vue';
import { NyaLinkClient, type ConnState, type DeviceInfo } from '@nyabula/nyalink';
import { relayUrl } from '../api/cloud';

export type Transport = 'lan' | 'cloud' | 'dev';

export interface KnownDevice {
  key: string;
  transport: Transport;
  label: string;
  /** lan: host:port; cloud: deviceId */
  address: string;
  lastSeen: number;
  deviceId?: string;
  coreVersion?: string;
}

const KNOWN_KEY = 'nyabula.devices';
const LAST_KEY = 'nyabula.lastDevice';
const TOKEN_PREFIX = 'nyalink.token:';

export function parseDeviceKey(key: string): { transport: Transport; address: string } | null {
  const i = key.indexOf(':');
  if (i < 0) return null;
  const transport = key.slice(0, i);
  const address = key.slice(i + 1);
  if ((transport !== 'lan' && transport !== 'cloud' && transport !== 'dev') || !address) return null;
  return { transport, address };
}

export function lanKey(host: string, port = 7788): string {
  return `lan:${host}:${port}`;
}
export function cloudKey(deviceId: string): string {
  return `cloud:${deviceId}`;
}

function wsUrlFor(key: string): string | null {
  const parsed = parseDeviceKey(key);
  if (!parsed) return null;
  if (parsed.transport === 'dev') return null;
  if (parsed.transport === 'cloud') return relayUrl(parsed.address);
  // lan: allow "host:port" or a full ws:// URL
  if (/^wss?:\/\//.test(parsed.address)) return parsed.address;
  return `ws://${parsed.address}/nyalink`;
}

function loadKnown(): KnownDevice[] {
  try {
    const raw = localStorage.getItem(KNOWN_KEY);
    if (raw) return JSON.parse(raw) as KnownDevice[];
  } catch {
    /* ignore */
  }
  return [];
}

export const useSessionStore = defineStore('session', () => {
  const client = shallowRef<NyaLinkClient | null>(null);
  const deviceKey = ref<string | null>(null);
  const state = ref<ConnState>('idle');
  const device = ref<DeviceInfo | null>(null);
  const role = ref<'owner' | 'family' | 'guest' | null>(null);
  const lastError = ref<string | null>(null);
  const known = ref<KnownDevice[]>(loadKnown());
  const lastDeviceKey = ref<string | null>(localStorage.getItem(LAST_KEY));

  const transport = computed<Transport | null>(() => (deviceKey.value ? parseDeviceKey(deviceKey.value)?.transport ?? null : null));
  const connected = computed(() => state.value === 'connected');
  const online = computed(() => state.value === 'connected' || state.value === 'pairing-required' || state.value === 'authenticating');
  const isOwner = computed(() => role.value === 'owner');
  const authRequired = computed(() => state.value === 'closed' && !!client.value?.authError);
  const canControl = computed(() => connected.value && (role.value === 'owner' || role.value === 'family'));

  function persistKnown(): void {
    localStorage.setItem(KNOWN_KEY, JSON.stringify(known.value.slice(0, 20)));
  }

  function remember(key: string, patch: Partial<KnownDevice> = {}): void {
    const parsed = parseDeviceKey(key);
    if (!parsed) return;
    const existing = known.value.find((d) => d.key === key);
    const entry: KnownDevice = existing ?? {
      key,
      transport: parsed.transport,
      address: parsed.address,
      label: parsed.address,
      lastSeen: Date.now(),
    };
    Object.assign(entry, patch, { lastSeen: Date.now() });
    known.value = [entry, ...known.value.filter((d) => d.key !== key)];
    persistKnown();
  }

  function forgetKnown(key: string): void {
    known.value = known.value.filter((d) => d.key !== key);
    persistKnown();
    const url = wsUrlFor(key);
    if (url) localStorage.removeItem(TOKEN_PREFIX + tokenScope(key, url));
  }

  /** Token storage scope: cloud keys rotate the session token in the URL, so key by deviceId. */
  function tokenScope(key: string, url: string): string {
    const parsed = parseDeviceKey(key);
    return parsed?.transport === 'cloud' ? `cloud:${parsed.address}` : url;
  }

  function connect(key: string, accessToken?: string): boolean {
    const url = wsUrlFor(key);
    if (!url) return false;
    if (client.value && deviceKey.value === key && (state.value === 'connected' || state.value === 'connecting' || state.value === 'authenticating')) {
      return true;
    }
    disconnect();
    deviceKey.value = key;
    lastError.value = null;
    const scope = tokenScope(key, url);
    const c = new NyaLinkClient({
      clientKind: 'web',
      version: '0.2.0',
      loadToken: () => accessToken || localStorage.getItem(TOKEN_PREFIX + scope),
      saveToken: (_u, token) => localStorage.setItem(TOKEN_PREFIX + scope, token),
    });
    c.onStateChange((s) => {
      state.value = s;
      if (s === 'closed' && c.authError) lastError.value = '认证失败，请重新输入设备令牌';
      if (s === 'connected') {
        device.value = c.device;
        role.value = (c.role as typeof role.value) ?? null;
        remember(key, { label: c.device?.name ?? undefined, deviceId: c.device?.id, coreVersion: c.device?.coreVersion });
        localStorage.setItem(LAST_KEY, key);
        lastDeviceKey.value = key;
      }
    });
    client.value = c;
    c.connect(url);
    remember(key);
    return true;
  }

  async function pair(code: string, name = 'Nyabula Web'): Promise<void> {
    lastError.value = null;
    try {
      await client.value?.pair(code, name);
    } catch (e) {
      lastError.value = e instanceof Error ? e.message : String(e);
      throw e;
    }
  }

  /** Developer preview: address a virtual device without any transport. */
  function enterPreview(key: string): void {
    disconnect();
    deviceKey.value = key;
  }

  function disconnect(): void {
    client.value?.close();
    client.value = null;
    state.value = 'idle';
    device.value = null;
    role.value = null;
  }

  function forgetCurrent(): void {
    const key = deviceKey.value;
    disconnect();
    if (key) forgetKnown(key);
    deviceKey.value = null;
  }

  /** Request that surfaces errors. Rejects immediately while offline. */
  function request(topic: string, data: Record<string, unknown> = {}): Promise<Record<string, unknown>> {
    const c = client.value;
    if (!c || state.value !== 'connected') {
      const preview = deviceKey.value?.startsWith('dev:');
      return Promise.reject(Object.assign(new Error(preview ? '开发预览：未连接设备' : '设备未连接'), { code: preview ? 'EPREVIEW' : 'EOFFLINE' }));
    }
    return c.request(topic, data);
  }

  function onEvent(topic: string, cb: (data: Record<string, unknown>) => void): () => void {
    const c = client.value;
    if (!c) return () => undefined;
    return c.on(topic, cb);
  }

  const clockOffsetMs = () => client.value?.clockOffsetMs ?? 0;

  return {
    client,
    deviceKey,
    transport,
    state,
    device,
    role,
    lastError,
    known,
    lastDeviceKey,
    connected,
    online,
    isOwner,
    authRequired,
    canControl,
    connect,
    pair,
    disconnect,
    enterPreview,
    forgetCurrent,
    forgetKnown,
    remember,
    request,
    onEvent,
    clockOffsetMs,
  };
});
