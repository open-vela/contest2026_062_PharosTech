/*
 * Nyabula Cloud HTTP API client (/api/v1).
 * Contract: Shared/protocol/cloud.md §4 — treat the doc as the source of
 * truth; the Go backend is developed in parallel against the same doc.
 *
 * - Bearer session token persisted in localStorage.
 * - Unified error shape {error:{code,message}} mapped to ApiError.
 */

const TOKEN_KEY = 'nyacloud.token';
const BASE = '/api/v1';

export interface User {
  id?: string;
  email: string;
  name: string;
}

export interface Device {
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

export function getToken(): string | null {
  return localStorage.getItem(TOKEN_KEY);
}

export function setToken(token: string | null): void {
  if (token) localStorage.setItem(TOKEN_KEY, token);
  else localStorage.removeItem(TOKEN_KEY);
}

/** Build the relay WebSocket URL for a device (cloud.md §3). */
export function relayUrl(deviceId: string): string {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const token = getToken() ?? '';
  return `${proto}://${location.host}${BASE}/relay/${encodeURIComponent(deviceId)}?token=${encodeURIComponent(token)}`;
}

async function req<T>(method: string, path: string, body?: unknown): Promise<T> {
  const headers: Record<string, string> = {};
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  const token = getToken();
  if (token) headers['Authorization'] = `Bearer ${token}`;

  let resp: Response;
  try {
    resp = await fetch(BASE + path, {
      method,
      headers,
      body: body !== undefined ? JSON.stringify(body) : undefined,
    });
  } catch {
    throw new ApiError('ENETWORK', '无法连接 Cloud 服务器', 0);
  }

  let data: unknown = null;
  const text = await resp.text();
  if (text) {
    try {
      data = JSON.parse(text);
    } catch {
      /* non-JSON body tolerated */
    }
  }

  if (!resp.ok) {
    const err = (data as { error?: { code?: string; message?: string } } | null)?.error;
    throw new ApiError(err?.code ?? `EHTTP${resp.status}`, err?.message ?? resp.statusText, resp.status);
  }
  return (data ?? {}) as T;
}

/* ---------------- auth ---------------- */

export function authRegister(email: string, password: string, name: string) {
  return req<{ token: string; user: User }>('POST', '/auth/register', { email, password, name });
}

export function authLogin(email: string, password: string) {
  return req<{ token: string; user: User }>('POST', '/auth/login', { email, password });
}

export function authMe() {
  return req<{ user: User }>('GET', '/auth/me');
}

/* ---------------- devices ---------------- */

export function listDevices() {
  return req<{ devices: Device[] }>('GET', '/devices');
}

export function claimDevice(deviceId: string, claimCode: string) {
  return req<{ device: Device }>('POST', '/devices/claim', { deviceId, claimCode });
}

export function unclaimDevice(deviceId: string) {
  return req<Record<string, never>>('DELETE', `/devices/${encodeURIComponent(deviceId)}`);
}

export function deviceStats(deviceId: string) {
  return req<DeviceStats>('GET', `/devices/${encodeURIComponent(deviceId)}/stats`);
}

/* ---------------- stats ---------------- */

export function statsOverview() {
  return req<Overview>('GET', '/stats/overview');
}
