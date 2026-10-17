/* WiFi provisioning helpers (pure): network.status parsing, scan result
 * normalisation and credential validation. */

export type NetworkState = 'idle' | 'ap_provision' | 'sta_connecting' | 'sta_online' | 'sta_failed';

export interface NetworkStatus {
  state: NetworkState | null;
  ifname: string | null;
  configured: boolean;
  error: string | null;
  attempt: number;
  /** Stored station SSID (present when configured). */
  ssid: string | null;
  /** Station address (present when online). */
  ipv4: string | null;
  /** Station signal in dBm (negative; only while online and known). */
  rssi: number | null;
  /** Name the device announces to the router (DHCP hostname). */
  hostname: string | null;
  ap: { ssid: string | null; ipv4: string | null } | null;
}

export interface WifiNetwork {
  ssid: string;
  rssi: number | null;
  freq: number | null;
  /** null = the device did not say; both open and protected are accepted. */
  secure: boolean | null;
}

const STATES: readonly string[] = ['idle', 'ap_provision', 'sta_connecting', 'sta_online', 'sta_failed'];

export const NETWORK_STATE_LABEL: Record<NetworkState, string> = {
  idle: '未配置网络',
  ap_provision: '配网热点已开启',
  sta_connecting: '正在连接 WiFi',
  sta_online: '已连接 WiFi',
  sta_failed: '连接 WiFi 失败',
};

function str(v: unknown): string | null {
  return typeof v === 'string' && v.length > 0 ? v : null;
}
function num(v: unknown): number | null {
  return typeof v === 'number' && Number.isFinite(v) ? v : null;
}
function isRecord(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}

export function parseNetworkStatus(raw: unknown): NetworkStatus {
  const d = isRecord(raw) ? raw : {};
  const ap = isRecord(d.ap) ? d.ap : null;
  return {
    state: typeof d.state === 'string' && STATES.includes(d.state) ? (d.state as NetworkState) : null,
    ifname: str(d.ifname),
    configured: d.configured === true,
    error: str(d.error),
    attempt: num(d.attempt) ?? 0,
    ssid: str(d.ssid),
    ipv4: str(d.ipv4),
    rssi: num(d.rssi),
    hostname: str(d.hostname),
    ap: ap ? { ssid: str(ap.ssid), ipv4: str(ap.ipv4) } : null,
  };
}

/** The device fell back to its hotspot although credentials are stored. */
export function joinFailed(status: NetworkStatus): boolean {
  return status.configured && (status.state === 'ap_provision' || status.state === 'sta_failed');
}

function secureOf(entry: Record<string, unknown>): boolean | null {
  if (typeof entry.secure === 'boolean') return entry.secure;
  if (typeof entry.secure === 'number') return entry.secure !== 0;
  const auth = entry.security ?? entry.auth;
  if (typeof auth === 'string') return !/^(open|none|)$/i.test(auth.trim());
  return null;
}

/** Locate the network list: a top-level array, `networks`, or the first array-valued field. */
function scanEntries(raw: unknown): unknown[] {
  if (Array.isArray(raw)) return raw;
  if (!isRecord(raw)) return [];
  if (Array.isArray(raw.networks)) return raw.networks;
  for (const value of Object.values(raw)) if (Array.isArray(value)) return value;
  return [];
}

/** Drop hidden networks, keep the strongest entry per SSID, sort by signal. */
export function normalizeScan(raw: unknown): WifiNetwork[] {
  const best = new Map<string, WifiNetwork>();
  for (const entry of scanEntries(raw)) {
    if (!isRecord(entry) || typeof entry.ssid !== 'string') continue;
    const ssid = entry.ssid.replace(/\0/g, '');
    if (!ssid.trim()) continue;
    const next: WifiNetwork = { ssid, rssi: num(entry.rssi), freq: num(entry.freq), secure: secureOf(entry) };
    const prev = best.get(ssid);
    if (!prev || (next.rssi ?? -Infinity) > (prev.rssi ?? -Infinity)) best.set(ssid, next);
  }
  return [...best.values()].sort((a, b) => (b.rssi ?? -Infinity) - (a.rssi ?? -Infinity) || a.ssid.localeCompare(b.ssid));
}

export function bandLabel(freq: number | null): string {
  if (freq === null) return '';
  return freq >= 5925 ? '6 GHz' : freq >= 4900 ? '5 GHz' : '2.4 GHz';
}

/** Returns a user-facing message, or null when the credentials can be sent.
 *  `secure`: true = password required, false = open, null = unknown (manual SSID). */
export function validateWifiInput(ssid: string, psk: string, secure: boolean | null): string | null {
  if (!ssid) return '请选择或输入 WiFi 名称';
  if (new TextEncoder().encode(ssid).length > 32) return 'WiFi 名称过长（最多 32 字节）';
  if (secure === false) return null;
  if (!psk) return secure ? '请输入 WiFi 密码' : null;
  if (psk.length < 8 || psk.length > 63) return 'WiFi 密码长度应为 8–63 个字符';
  return null;
}

/** The request was sent but the socket went away before the answer arrived.
 *  (ENOTCONN / EOFFLINE mean it was never sent, so they do not count.) */
export function isLinkDropError(e: unknown): boolean {
  const code = isRecord(e) ? e.code : undefined;
  return code === 'ECONNRESET' || code === 'ETIMEDOUT';
}
