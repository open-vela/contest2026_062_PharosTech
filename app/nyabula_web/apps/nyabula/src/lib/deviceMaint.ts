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
/** `active` is the slot bootctrl will start next; `running` the one that is
 *  executing now. They differ between an update and the reboot after it.
 *  There is no retry counter: the boot loader selects by priority alone. */
export interface UpdateSlot {
  name: SlotName;
  active: boolean;
  running: boolean;
  bootable: boolean;
  successful: boolean;
  priority: number;
  version: number;
  size: number;
}
export type ApplyState = 'idle' | 'writing' | 'done' | 'failed';
export interface UpdateApply {
  state: ApplyState;
  /** errno of the last failure (positive), 0 when there is none. */
  error: number;
  /** Device's short word for it: digest / verify / too-large / io / ... */
  reason: string;
}
export interface UpdateStatus {
  current: { version: string; builtAt: string; slot: SlotName | '' };
  slots: UpdateSlot[];
  channel: string;
  online: boolean;
  detail: string;
  /** The firmware takes an image over POST /ota/upload. */
  upload: boolean;
  /** Largest image it takes, in bytes; 0 when it takes none. */
  maxBytes: number;
  /** Slot an upload would be written to: never the running one. */
  target: SlotName | '';
  apply: UpdateApply;
}

const APPLY_STATES: readonly string[] = ['idle', 'writing', 'done', 'failed'];

function slotName(v: unknown): SlotName | '' {
  const s = str(v).toLowerCase();
  return s === 'a' || s === 'b' ? s : '';
}

export function parseUpdateStatus(raw: unknown): UpdateStatus {
  const d = isRecord(raw) ? raw : {};
  const cur = isRecord(d.current) ? d.current : {};
  const running = slotName(cur.slot);
  const slots: UpdateSlot[] = [];
  for (const s of records(d.slots)) {
    const name = slotName(s.name);
    if (!name || slots.some((x) => x.name === name)) continue;
    slots.push({
      name,
      active: s.active === true,
      // Older firmware reported no `running`; the current slot says the same.
      running: typeof s.running === 'boolean' ? s.running : name === running,
      bootable: s.bootable === true,
      successful: s.successful === true,
      priority: size(s.priority),
      version: size(s.version),
      size: size(s.size),
    });
  }
  const ap = isRecord(d.apply) ? d.apply : {};
  const upload = d.upload === true;
  const target = slotName(d.target) || (upload && running ? (running === 'a' ? 'b' : 'a') : '');
  return {
    current: { version: str(cur.version), builtAt: str(cur.builtAt), slot: running },
    slots,
    channel: str(d.channel) || 'manual',
    online: d.online === true,
    detail: str(d.detail),
    upload,
    maxBytes: upload ? size(d.maxBytes) : 0,
    target,
    apply: {
      state: typeof ap.state === 'string' && APPLY_STATES.includes(ap.state) ? (ap.state as ApplyState) : 'idle',
      error: size(ap.error),
      reason: str(ap.reason),
    },
  };
}

/** The running slot has booted but nobody has said it works yet. */
export function needsConfirm(status: UpdateStatus): boolean {
  return status.upload && status.slots.some((s) => s.running && !s.successful);
}

/** An update is in place and waits for a reboot: the slot that starts next is
 *  bootable and is not the one running. */
export function pendingReboot(status: UpdateStatus): SlotName | '' {
  return status.slots.find((s) => s.active && !s.running && s.bootable)?.name ?? '';
}

/* ---- POST /ota/upload ---- */

/** Where the image goes and where this browser keeps the token for it.
 *
 *  The upload is plain HTTP next to the WebSocket, so it only works against
 *  the origin that served the page: the device answers no CORS preflight, and
 *  an Authorization header forces one everywhere else. `null` = not that case.
 *  `tokenSlot` mirrors the session store's localStorage key for the socket. */
export function otaEndpoint(deviceKey: string | null, loc: { protocol: string; host: string }): { url: string; tokenSlot: string } | null {
  if (!deviceKey || (loc.protocol !== 'http:' && loc.protocol !== 'https:')) return null;
  const sameHost = deviceKey === 'self' || deviceKey === `lan:${loc.host}`;
  if (!sameHost) return null;
  const ws = `${loc.protocol === 'https:' ? 'wss' : 'ws'}://${loc.host}/nyalink`;
  return { url: `${loc.protocol}//${loc.host}/ota/upload`, tokenSlot: `nyalink.token:${ws}` };
}

/** arm64 Image header: "ARM\x64" at byte 56. The device checks the same. */
export function isFirmwareImage(head: Uint8Array): boolean {
  return head.length >= 60 && head[56] === 0x41 && head[57] === 0x52 && head[58] === 0x4d && head[59] === 0x64;
}

export interface UploadReply {
  ok: boolean;
  received: number;
  sha256: string;
  /** Device error word (EAUTH, EBUSY, ENOSPACE, ...), '' when none. */
  error: string;
}

/** Parse the JSON answer of /ota/upload. A body that is not JSON (a proxy's
 *  page, an old firmware's index.html) is a failure, never a success. */
export function parseUploadReply(httpStatus: number, body: string): UploadReply {
  let d: Record<string, unknown> = {};
  let json = false;
  try {
    const parsed: unknown = JSON.parse(body);
    if (isRecord(parsed)) {
      d = parsed;
      json = true;
    }
  } catch {
    /* not JSON */
  }
  const sha256 = str(d.sha256).toLowerCase();
  const ok = json && httpStatus === 200 && /^[0-9a-f]{64}$/.test(sha256);
  return { ok, received: size(d.received), sha256, error: ok ? '' : str(d.error) || (json ? '' : 'ENOTJSON') };
}

const UPLOAD_ERRORS: Record<number, string> = {
  0: '连接中断，固件没有传完',
  401: '登录凭据已失效，请重新用密码登录后再试',
  404: '此固件不支持网页上传',
  // What a firmware without /ota/ answers to a POST: its file server only reads.
  405: '此固件不支持网页上传',
  408: '传输停滞超时，设备已放弃这次上传',
  409: '设备正在处理另一次上传或写入，请稍后再试',
  411: '浏览器没有发送文件大小，无法上传',
  413: '文件太大，或设备 /data 剩余空间不足',
  415: '这不是 NuttX 固件镜像（缺少 arm64 Image 头）',
  422: '设备收到的数据与本机校验值不一致，已丢弃，请重试',
  507: '设备存储空间不足',
};

export function uploadErrorText(httpStatus: number, reply: UploadReply): string {
  if (reply.error === 'ENOTJSON' && httpStatus === 200) return UPLOAD_ERRORS[404]!;
  return UPLOAD_ERRORS[httpStatus] ?? `上传失败（HTTP ${httpStatus}${reply.error ? ` ${reply.error}` : ''}）`;
}

const APPLY_REASONS: Record<string, string> = {
  digest: '暂存的固件与确认时的校验值不一致，已丢弃',
  verify: '写入后从存储回读的内容不一致，新槽位保持不可启动',
  'too-large': '固件比槽位分区大',
  io: '读写存储失败',
  'staged-file': '读不到已上传的固件，请重新上传',
  memory: '设备内存不足，无法开始写入',
};

export function applyErrorText(apply: UpdateApply): string {
  const text = APPLY_REASONS[apply.reason] ?? '写入失败';
  return apply.error ? `${text}（错误码 ${apply.error}）` : text;
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
