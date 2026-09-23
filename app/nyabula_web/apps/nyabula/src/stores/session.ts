/* Device session: one NyaLink connection at a time, addressed by a deviceKey.
 *   lan:<host>:<port>   direct LAN WebSocket
 *   cloud:<deviceId>    Cloud relay (identical NyaLink semantics)
 *   self                the device that served this page (same host and port)
 * Pages never branch on the transport; only the URL builder does.
 * Also keeps the "known devices" list for the connect screen. */
import { defineStore } from 'pinia';
import { computed, ref, shallowRef } from 'vue';
import { NyaLinkClient, NyaLinkError, type ConnState, type DeviceInfo, type HelloResult, type RequestOptions } from '@nyabula/nyalink';
import { relayUrl } from '../api/cloud';
import { SELF_KEY, parseDeviceKey, selfWsUrl, type Transport } from '../lib/deviceKey';
import { isDeviceToken } from '../lib/deviceToken';

export { SELF_KEY, cloudKey, lanKey, parseDeviceKey, type Transport } from '../lib/deviceKey';

export interface KnownDevice {
  key: string;
  transport: Transport;
  label: string;
  /** lan: host:port; cloud: deviceId; self: host the page was served from */
  address: string;
  lastSeen: number;
  deviceId?: string;
  coreVersion?: string;
}

const KNOWN_KEY = 'nyabula.devices';
const LAST_KEY = 'nyabula.lastDevice';
const TOKEN_PREFIX = 'nyalink.token:';
/** How long preAuth() waits for a fresh socket to open. */
const SOCKET_OPEN_TIMEOUT_MS = 8000;

/** Which credential a socket said (or will say) hello with. */
export type CredentialKind = 'pair' | 'session';

export interface ConnectOptions {
  /** Device build only: open the socket without saying hello, even when a
   *  credential is at hand, so the caller can run pre-auth requests first. */
  deferAuth?: boolean;
}

function wsUrlFor(key: string): string | null {
  const parsed = parseDeviceKey(key);
  if (!parsed) return null;
  if (parsed.transport === 'dev') return null;
  if (parsed.transport === 'cloud') return relayUrl(parsed.address);
  if (parsed.transport === 'self') return selfWsUrl(location);
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
  /* Device build: the pair token from the QR-code URL lives in this closure
   * only (never storage, never reactive state that devtools could show). */
  let pairToken: string | null = null;
  let activeToken: string | null = null;
  const hasPairToken = ref(false);
  /** Credential of the current (or last attempted) hello, as chosen locally. */
  const credential = ref<CredentialKind | null>(null);
  /** `auth` of the live socket as the device reported it in sys.hello (it is
   *  the device that enforces what a pair / session socket may do). */
  const helloAuth = ref<CredentialKind | null>(null);
  /** `passwordSet` as last reported by sys.hello / a password change. */
  const passwordSet = ref<boolean | null>(null);

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
    const address = parsed.transport === 'self' ? location.host : parsed.address;
    const entry: KnownDevice = existing ?? {
      key,
      transport: parsed.transport,
      address,
      label: address,
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

  /** Adopt the pair token from the QR-code URL for this tab only. It is never
   *  persisted: what gets stored is the session token of a login / password
   *  change. Returns false when the value is not a well-formed device token. */
  function adoptPairToken(token: string): boolean {
    if (!isDeviceToken(token)) return false;
    pairToken = token;
    hasPairToken.value = true;
    return true;
  }

  function dropPairToken(): void {
    pairToken = null;
    hasPairToken.value = false;
  }

  function tokenSlot(key: string): string | null {
    const url = wsUrlFor(key);
    return url ? TOKEN_PREFIX + tokenScope(key, url) : null;
  }

  function hasToken(key: string): boolean {
    const slot = tokenSlot(key);
    return !!slot && !!localStorage.getItem(slot);
  }

  /** Persist the session token of a login / password change and make the live
   *  client use it for its next hello (reconnects included). */
  function storeSessionToken(key: string, token: string): boolean {
    const slot = tokenSlot(key);
    if (!slot || !isDeviceToken(token)) return false;
    localStorage.setItem(slot, token);
    if (deviceKey.value === key) {
      client.value?.setToken(token);
      activeToken = token;
      credential.value = 'session';
    }
    return true;
  }

  /** Forget the stored session token. With `onlyRejected`, only when it is
   *  the one the device just refused: another tab may have stored a newer
   *  one after a password change. Returns whether a stored token remains. */
  function forgetSessionToken(key: string, onlyRejected = false): boolean {
    const slot = tokenSlot(key);
    if (!slot) return false;
    const stored = localStorage.getItem(slot);
    if (stored && onlyRejected && stored !== activeToken) return true;
    localStorage.removeItem(slot);
    return false;
  }

  /** Device build: the page talks to the device that served it, and owns the
   *  hello itself (pre-auth requests, password login). */
  function isManual(key: string): boolean {
    return __NYA_DEVICE__ && key === SELF_KEY;
  }

  function pickCredential(key: string): { kind: CredentialKind; token: string } | null {
    if (pairToken) return { kind: 'pair', token: pairToken };
    const slot = tokenSlot(key);
    const stored = slot ? localStorage.getItem(slot) : null;
    return stored ? { kind: 'session', token: stored } : null;
  }

  function connect(key: string, accessToken?: string, options: ConnectOptions = {}): boolean {
    const url = wsUrlFor(key);
    if (!url) return false;
    if (client.value && deviceKey.value === key && (state.value === 'connected' || state.value === 'connecting' || state.value === 'authenticating')) {
      return true;
    }
    disconnect();
    deviceKey.value = key;
    lastError.value = null;
    const scope = tokenScope(key, url);
    const manual = isManual(key);
    const c = new NyaLinkClient({
      clientKind: 'web',
      version: '0.2.0',
      manualHello: manual,
      loadToken: () => accessToken || localStorage.getItem(TOKEN_PREFIX + scope),
      saveToken: (_u, token) => localStorage.setItem(TOKEN_PREFIX + scope, token),
    });
    credential.value = null;
    activeToken = null;
    if (manual && !options.deferAuth) {
      // Reconnect / retry / deep link: say hello right away with what we hold.
      const picked = pickCredential(key);
      if (picked) {
        c.setToken(picked.token);
        activeToken = picked.token;
        credential.value = picked.kind;
      }
    }
    c.onStateChange((s) => {
      if (client.value !== c) return;
      state.value = s;
      if (s === 'closed' && c.authError) lastError.value = __NYA_DEVICE__ ? '登录已失效，请重新登录' : '认证失败，请重新输入设备令牌';
      if (s === 'connected') {
        if (c.passwordSet !== null) passwordSet.value = c.passwordSet;
        helloAuth.value = c.auth;
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

  /** Resolve once `c` has an open socket that waits for hello. */
  function socketAwaitingAuth(c: NyaLinkClient): Promise<void> {
    return new Promise((resolve, reject) => {
      const done = (err?: Error): void => {
        off();
        clearTimeout(timer);
        if (err) reject(err);
        else resolve();
      };
      const check = (): void => {
        if (c.awaitingAuth) done();
        else if (c.state === 'closed' || client.value !== c) done(new NyaLinkError('ECONNRESET', '设备连接已关闭'));
      };
      const timer = setTimeout(() => done(new NyaLinkError('ETIMEDOUT', '连接设备超时')), SOCKET_OPEN_TIMEOUT_MS);
      const off = c.onStateChange(check);
      check();
    });
  }

  /** Device build: a request on a socket that has not said hello
   *  (sys.auth.state, sys.login). The device closes such a socket after a
   *  short idle time and allows only a few sockets in total, so one is opened
   *  here on demand (replacing any other) and the caller lets go of it with
   *  releasePreAuth() as soon as it is done. */
  async function preAuth(key: string, topic: string, data: Record<string, unknown> = {}, options?: RequestOptions): Promise<Record<string, unknown>> {
    let c = client.value;
    // Reuse a socket that waits for hello (or is still opening for that purpose).
    const reusable = !!c && deviceKey.value === key && !activeToken && (c.awaitingAuth || c.state === 'connecting' || c.state === 'reconnecting');
    if (!c || !reusable) {
      disconnect();
      if (!connect(key, undefined, { deferAuth: true })) throw new NyaLinkError('ENOTCONN', '无法连接设备');
      c = client.value!;
    }
    await socketAwaitingAuth(c);
    return c.request(topic, data, options);
  }

  /** Close a socket that never said hello; keep an authenticated one. */
  function releasePreAuth(): void {
    if (client.value && deviceKey.value && isManual(deviceKey.value) && !activeToken) disconnect();
  }

  /** Device build: say hello on the waiting socket with the pair token or the
   *  stored session token. Rejects with the NyaLinkError of the device. */
  async function authenticate(kind: CredentialKind): Promise<HelloResult> {
    const c = client.value;
    const slot = deviceKey.value ? tokenSlot(deviceKey.value) : null;
    const token = kind === 'pair' ? pairToken : slot ? localStorage.getItem(slot) : null;
    if (!c || !token) throw new NyaLinkError('ENOTCONN', '设备未连接');
    credential.value = kind;
    activeToken = token;
    return c.authenticate(token);
  }

  /** The device was renamed (network.name.set): refresh every place that
   *  shows the name without waiting for the next hello. */
  function setDeviceName(name: string): void {
    if (!name || !device.value) return;
    device.value = { ...device.value, name };
    // Keep the client copy in step: it is what a later state change reads.
    if (client.value?.device) client.value.device = { ...client.value.device, name };
    if (deviceKey.value) remember(deviceKey.value, { label: name });
  }

  function markPasswordSet(): void {
    passwordSet.value = true;
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
    const c = client.value;
    client.value = null;
    c?.close();
    state.value = 'idle';
    helloAuth.value = null;
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
  function request(topic: string, data: Record<string, unknown> = {}, options?: RequestOptions): Promise<Record<string, unknown>> {
    const c = client.value;
    if (!c || state.value !== 'connected') {
      const preview = deviceKey.value?.startsWith('dev:');
      return Promise.reject(Object.assign(new Error(preview ? '开发预览：未连接设备' : '设备未连接'), { code: preview ? 'EPREVIEW' : 'EOFFLINE' }));
    }
    return c.request(topic, data, options);
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
    hasPairToken,
    credential,
    helloAuth,
    passwordSet,
    adoptPairToken,
    dropPairToken,
    hasToken,
    storeSessionToken,
    forgetSessionToken,
    preAuth,
    releasePreAuth,
    authenticate,
    markPasswordSet,
    setDeviceName,
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
