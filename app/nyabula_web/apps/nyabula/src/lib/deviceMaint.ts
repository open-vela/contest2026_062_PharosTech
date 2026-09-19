/* Device maintenance payloads (pure): storage.status, update.status,
 * logs.tail and cloud.status parsing, plus the formatters the settings
 * sections share. Parsers never throw: missing fields fall back to neutral
 * values so an older / newer firmware cannot break a page. */

function isRecord(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}
function str(v: unknown): string {
  return typeof v === 'string' ? v : '';
}
function size(v: unknown): number {
  return typeof v === 'number' && Number.isFinite(v) && v > 0 ? v : 0;
}
function records(v: unknown): Record<string, unknown>[] {
  return Array.isArray(v) ? v.filter(isRecord) : [];
}

/** The device does not know the topic (older firmware). */
export function isUnsupportedError(e: unknown): boolean {
  return isRecord(e) && e.code === 'ENOTFOUND';
}

/** 1536 -> "1.5 KB"; binary units, at most one decimal. */
export function fmtBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  const text = unit === 0 || value >= 100 ? String(Math.round(value)) : value.toFixed(1).replace(/\.0$/, '');
  return `${text} ${units[unit]}`;
}

/* ---- storage.status ---- */

export interface StorageVolume {
  id: string;
  label: string;
  path: string;
  total: number;
  used: number;
  free: number;
  /** used / total in percent (0-100); 0 when the total is unknown. */
  percent: number;
}
export interface StorageUsage {
  id: string;
  label: string;
  path: string;
  bytes: number;
  clearable: boolean;
}
export interface StorageStatus {
  volumes: StorageVolume[];
  usage: StorageUsage[];
  /** Only in the answer of storage.cleanup. */
  freed: number | null;
}

export function parseStorageStatus(raw: unknown): StorageStatus {
  const d = isRecord(raw) ? raw : {};
  const volumes = records(d.volumes).map((v, i): StorageVolume => {
    const total = size(v.total);
    const free = Math.min(size(v.free), total || Infinity);
    // `used` may be absent; total - free is the fallback.
    const used = typeof v.used === 'number' ? size(v.used) : Math.max(0, total - free);
    const id = str(v.id) || str(v.path) || `volume-${i}`;
    return { id, label: str(v.label) || str(v.path) || id, path: str(v.path), total, used, free, percent: total > 0 ? Math.min(100, Math.round((used / total) * 100)) : 0 };
  });
  const usage = records(d.usage)
    .filter((u) => str(u.id) !== '')
    .map((u): StorageUsage => ({ id: str(u.id), label: str(u.label) || str(u.path) || str(u.id), path: str(u.path), bytes: size(u.bytes), clearable: u.clearable === true }));
  return { volumes, usage, freed: typeof d.freed === 'number' ? size(d.freed) : null };
}

/* ---- update.status ---- */

export type SlotName = 'a' | 'b';
export interface UpdateSlot {
  name: SlotName;
  active: boolean;
  bootable: boolean;
  successful: boolean;
  triesRemaining: number | null;
}
export interface UpdateStatus {
  current: { version: string; builtAt: string; slot: SlotName | '' };
  slots: UpdateSlot[];
  channel: string;
  online: boolean;
  detail: string;
}

function slotName(v: unknown): SlotName | '' {
  const s = str(v).toLowerCase();
  return s === 'a' || s === 'b' ? s : '';
}

export function parseUpdateStatus(raw: unknown): UpdateStatus {
  const d = isRecord(raw) ? raw : {};
  const cur = isRecord(d.current) ? d.current : {};
  const slots: UpdateSlot[] = [];
  for (const s of records(d.slots)) {
    const name = slotName(s.name);
    if (!name || slots.some((x) => x.name === name)) continue;
    slots.push({
      name,
      active: s.active === true,
      bootable: s.bootable === true,
      successful: s.successful === true,
      triesRemaining: typeof s.triesRemaining === 'number' && Number.isFinite(s.triesRemaining) ? s.triesRemaining : null,
    });
  }
  return {
    current: { version: str(cur.version), builtAt: str(cur.builtAt), slot: slotName(cur.slot) },
    slots,
    channel: str(d.channel) || 'manual',
    online: d.online === true,
    detail: str(d.detail),
  };
}

/* ---- logs.tail ---- */

export interface LogLine {
  seq: number;
  text: string;
  /** Client-side marker (gap / restart), not a device line. */
  mark?: boolean;
}
export interface LogsTail {
  lines: LogLine[];
  /** Cursor for the next request (`after`); null when the device sent none. */
  next: number | null;
  dropped: boolean;
}

/** ANSI colour sequences and stray control characters (tabs are kept). */
const ANSI_SEQUENCE = new RegExp(String.fromCharCode(27) + '[[][0-9;?]*[A-Za-z]', 'g');

/** Drop ANSI sequences, then every control character except TAB. */
export function cleanLogText(text: string): string {
  const plain = text.replace(ANSI_SEQUENCE, '');
  let out = '';
  for (let i = 0; i < plain.length; i++) {
    const c = plain.charCodeAt(i);
    if (c === 9 || (c >= 0x20 && c !== 0x7f)) out += plain[i];
  }
  return out;
}

export function parseLogsTail(raw: unknown): LogsTail {
  const d = isRecord(raw) ? raw : {};
  const lines: LogLine[] = [];
  for (const l of records(d.lines)) {
    if (typeof l.seq !== 'number' || !Number.isFinite(l.seq)) continue;
    lines.push({ seq: l.seq, text: cleanLogText(str(l.text)) });
  }
  const last = lines.length ? lines[lines.length - 1]!.seq : null;
  return { lines, next: typeof d.next === 'number' && Number.isFinite(d.next) ? d.next : last, dropped: d.dropped === true };
}

/** Merge one logs.tail answer into the view and keep the newest `max` lines.
 *  Only lines newer than the last one held are taken. A gap the device
 *  reported (`dropped`) or a sequence restart (device rebooted: `restarted`,
 *  which also discards the old view) leaves a marker line in front of the
 *  new lines; its fractional seq keeps the list keys unique and ordered. */
export function mergeLogLines(held: LogLine[], tail: LogsTail, max: number, restarted = false): LogLine[] {
  const base = restarted ? [] : held;
  const lastSeq = base.length ? base[base.length - 1]!.seq : -Infinity;
  const fresh = tail.lines.filter((l) => l.seq > lastSeq);
  if (!fresh.length) return base;
  const marker: LogLine[] = restarted
    ? [{ seq: fresh[0]!.seq - 0.5, text: '—— 设备已重启，日志从头开始 ——', mark: true }]
    : tail.dropped && base.length
      ? [{ seq: fresh[0]!.seq - 0.5, text: '—— 中间有日志已被设备的环形缓冲覆盖 ——', mark: true }]
      : [];
  const merged = base.concat(marker, fresh);
  return merged.length > max ? merged.slice(merged.length - max) : merged;
}

/* ---- cloud.status / cloud.config ---- */

export type CloudState = 'disabled' | 'offline' | 'connecting' | 'online' | 'unsupported';
export interface CloudStatus {
  enabled: boolean;
  url: string;
  state: CloudState;
  detail: string;
  /** Cloud-side identity: only relays that implement claiming report these. */
  deviceId: string | null;
  claimCode: string | null;
}

const CLOUD_STATES: readonly string[] = ['disabled', 'offline', 'connecting', 'online', 'unsupported'];

/** Device contract: {enabled,url,state,detail}. The earlier shape
 *  {enabled,url,connected,deviceId?,claimCode?} is still understood. */
export function parseCloudStatus(raw: unknown): CloudStatus {
  const d = isRecord(raw) ? raw : {};
  const enabled = d.enabled === true;
  const state: CloudState = typeof d.state === 'string' && CLOUD_STATES.includes(d.state)
    ? (d.state as CloudState)
    : !enabled ? 'disabled' : d.connected === true ? 'online' : 'offline';
  return { enabled, url: str(d.url), state, detail: str(d.detail), deviceId: str(d.deviceId) || null, claimCode: str(d.claimCode) || null };
}
