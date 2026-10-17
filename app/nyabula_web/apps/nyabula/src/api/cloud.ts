/* Nyabula Cloud HTTP API client (/api/v1). Contract: Shared/protocol/cloud.md.
 * Bearer token persisted in localStorage; errors normalised to ApiError. */

const TOKEN_KEY = 'nyacloud.token';
const BASE = '/api/v1';

export interface User {
  id?: string;
  email: string;
  name: string;
}
export interface CloudDevice {
  deviceId: string;
  name: string;
  online: boolean;
  claimedAt: string;
  coreVersion: string;
  lastSeen: string;
}
export interface DailyStat {
  date: string;
  onlineSeconds: number;
  framesUp: number;
  framesDown: number;
}
export interface DeviceStats {
  daily: DailyStat[];
  current: { online: boolean; clients: number };
}
export interface Overview {
  devices: number;
  online: number;
  clients: number;
  framesToday: number;
}

export class ApiError extends Error {
  constructor(
    public code: string,
    message: string,
    public status: number,
  ) {
    super(message);
    this.name = 'ApiError';
  }
}

export function getCloudToken(): string | null {
  return localStorage.getItem(TOKEN_KEY);
}
export function setCloudToken(token: string | null): void {
  if (token) localStorage.setItem(TOKEN_KEY, token);
  else localStorage.removeItem(TOKEN_KEY);
}

/** Relay WebSocket URL for a claimed device (cloud.md §3). */
export function relayUrl(deviceId: string): string {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const token = getCloudToken() ?? '';
  return `${proto}://${location.host}${BASE}/relay/${encodeURIComponent(deviceId)}?token=${encodeURIComponent(token)}`;
}

async function req<T>(method: string, path: string, body?: unknown): Promise<T> {
  const headers: Record<string, string> = {};
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  const token = getCloudToken();
  if (token) headers['Authorization'] = `Bearer ${token}`;
  let resp: Response;
  try {
    resp = await fetch(BASE + path, { method, headers, body: body !== undefined ? JSON.stringify(body) : undefined });
  } catch {
    throw new ApiError('ENETWORK', '无法连接 Nyabula Cloud', 0);
  }
  let data: unknown = null;
  const text = await resp.text();
  if (text) {
    try {
      data = JSON.parse(text);
    } catch {
      /* tolerate non-JSON */
    }
  }
  if (!resp.ok) {
    const err = (data as { error?: { code?: string; message?: string } } | null)?.error;
    throw new ApiError(err?.code ?? `EHTTP${resp.status}`, err?.message ?? resp.statusText, resp.status);
  }
  return (data ?? {}) as T;
}

export const cloudApi = {
  register: (email: string, password: string, name: string) =>
    req<{ token: string; user: User }>('POST', '/auth/register', { email, password, name }),
  login: (email: string, password: string) => req<{ token: string; user: User }>('POST', '/auth/login', { email, password }),
  me: () => req<{ user: User }>('GET', '/auth/me'),
  listDevices: () => req<{ devices: CloudDevice[] }>('GET', '/devices'),
  claimDevice: (deviceId: string, claimCode: string) =>
    req<{ device: CloudDevice }>('POST', '/devices/claim', { deviceId, claimCode }),
  unclaimDevice: (deviceId: string) => req<Record<string, never>>('DELETE', `/devices/${encodeURIComponent(deviceId)}`),
  deviceStats: (deviceId: string) => req<DeviceStats>('GET', `/devices/${encodeURIComponent(deviceId)}/stats`),
  overview: () => req<Overview>('GET', '/stats/overview'),
};
