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
  /** Id of the target the last (or current) apply wrote; '' before the first. */
  target: string;
  /** It was written under a mounted filesystem: only a reboot makes it sane. */
  forced: boolean;
}

/** How an image reaches the medium: staged into an A/B slot with its
 *  bootctrl record, the boot loader in place, or a raw partition write. */
export type TargetKind = 'slot' | 'nboot' | 'partition';
/** What the device checks a file against before it takes it. */
export type TargetFormat = 'raw' | 'arm64' | 'fit' | 'bootctrl' | 'fat';
/** One thing the device can update. The list is the device's: the panel
 *  renders it and knows no target of its own. */
export interface UpdateTarget {
  id: string;
  label: string;
  description: string;
  kind: TargetKind;
  format: TargetFormat;
  /** Can leave the device unbootable; update.apply wants `advanced: true`. */
  advanced: boolean;
  /** Largest image it takes right now (partition and staging space). */
  maxBytes: number;
  /** Size of the partition alone. */
  capacity: number;
  /** Slot a staged image would replace; '' for the other kinds. */
  slot: SlotName | '';
  /** Whether update.apply would agree right now, and if not, why:
   *  running / blocked / no-handoff. */
  available: boolean;
  reason: string;
  /** A filesystem is mounted from it: update.apply wants `force: true`. */
  mounted: boolean;
}

export const DEFAULT_TARGET = 'nuttx';
export const AMP_TARGET = 'amp';

export interface UpdateStatus {
  current: { version: string; builtAt: string; slot: SlotName | '' };
  slots: UpdateSlot[];
  /** Slots of the AMP domain. None of them is ever `running`: the panel talks
   *  to the image of a NuttX slot. */
  ampSlots: UpdateSlot[];
  /** The boot loader will try the AMP domain first at the next start. */
  ampActive: boolean;
  /** Everything update.apply can write. A firmware from before there were
   *  targets gets the one it has. */
  targets: UpdateTarget[];
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

const TARGET_KINDS: readonly string[] = ['slot', 'nboot', 'partition'];
const TARGET_FORMATS: readonly string[] = ['raw', 'arm64', 'fit', 'bootctrl', 'fat'];

function parseSlots(raw: unknown, running: SlotName | '' | null): UpdateSlot[] {
  const slots: UpdateSlot[] = [];
  for (const s of records(raw)) {
    const name = slotName(s.name);
    if (!name || slots.some((x) => x.name === name)) continue;
    slots.push({
      name,
      active: s.active === true,
      // Older firmware reported no `running`; the current slot says the same.
      // `null` = a domain without a running slot (AMP): never inferred.
      running: typeof s.running === 'boolean' ? s.running : running !== null && name === running,
      bootable: s.bootable === true,
      successful: s.successful === true,
      priority: size(s.priority),
      version: size(s.version),
      size: size(s.size),
    });
  }
  return slots;
}

function parseTargets(raw: unknown): UpdateTarget[] {
  const targets: UpdateTarget[] = [];
  for (const t of records(raw)) {
    const id = str(t.id);
    if (!id || targets.some((x) => x.id === id)) continue;
    const kind = str(t.kind);
    const format = str(t.format);
    const maxBytes = size(t.maxBytes);
    targets.push({
      id,
      label: str(t.label) || id,
      description: str(t.description),
      // An unknown kind is treated as the most dangerous one it could be.
      kind: TARGET_KINDS.includes(kind) ? (kind as TargetKind) : 'partition',
      format: TARGET_FORMATS.includes(format) ? (format as TargetFormat) : 'raw',
      // Only the two the panel knows as ordinary may ever count as ordinary.
      advanced: t.advanced === true || (id !== DEFAULT_TARGET && id !== AMP_TARGET),
      maxBytes,
      capacity: size(t.capacity) || maxBytes,
      slot: slotName(t.slot),
      available: t.available !== false,
      reason: str(t.reason),
      mounted: t.mounted === true,
    });
  }
  return targets;
}

export function parseUpdateStatus(raw: unknown): UpdateStatus {
  const d = isRecord(raw) ? raw : {};
  const cur = isRecord(d.current) ? d.current : {};
  const running = slotName(cur.slot);
  const slots = parseSlots(d.slots, running);
  const ap = isRecord(d.apply) ? d.apply : {};
  const upload = d.upload === true;
  const target = slotName(d.target) || (upload && running ? (running === 'a' ? 'b' : 'a') : '');
  const maxBytes = upload ? size(d.maxBytes) : 0;
  let targets = upload ? parseTargets(d.targets) : [];
  if (upload && !targets.length) {
    // Firmware from before there were targets: it takes the NuttX image.
    targets = [{
      id: DEFAULT_TARGET, label: 'openvela 固件', description: '', kind: 'slot', format: 'arm64', advanced: false,
      maxBytes, capacity: maxBytes, slot: target, available: true, reason: '', mounted: false,
    }];
  }
  return {
    current: { version: str(cur.version), builtAt: str(cur.builtAt), slot: running },
    slots,
    ampSlots: parseSlots(d.ampSlots, null),
    ampActive: d.ampActive === true,
    targets,
    channel: str(d.channel) || 'manual',
    online: d.online === true,
    detail: str(d.detail),
    upload,
    maxBytes,
    target,
    apply: {
      state: typeof ap.state === 'string' && APPLY_STATES.includes(ap.state) ? (ap.state as ApplyState) : 'idle',
      error: size(ap.error),
      reason: str(ap.reason),
      target: str(ap.target),
      forced: ap.forced === true,
    },
  };
}

/** What the device said a forced write means (answer of update.apply). */
export function applyNotice(raw: unknown): string {
  return isRecord(raw) ? str(raw.notice) : '';
}

export function findTarget(status: UpdateStatus | null, id: string): UpdateTarget | null {
  return status?.targets.find((t) => t.id === id) ?? null;
}

/** An AMP image is in place and active, but nobody has said it works yet. */
export function needsAmpConfirm(status: UpdateStatus): boolean {
  return status.upload && status.ampSlots.some((s) => s.active && s.bootable && s.size > 0 && !s.successful);
}

/** The last apply wrote something the slot table does not show, and it only
 *  takes effect after a restart: its target, else null. The NuttX firmware is
 *  left to `pendingReboot`, which reads the same from the slots and survives
 *  a page that was opened later. */
export function appliedTarget(status: UpdateStatus): UpdateTarget | null {
  if (status.apply.state !== 'done' || !status.apply.target || status.apply.target === DEFAULT_TARGET) return null;
  return findTarget(status, status.apply.target);
}

const UNAVAILABLE: Record<string, string> = {
  running: '正在运行的固件就在这个分区里，不能覆盖',
  blocked: '不能从面板写入',
  'no-handoff': '设备不是由 N-Boot 启动的，无法确定启动介质',
};

export function unavailableText(target: UpdateTarget): string {
  return target.available ? '' : UNAVAILABLE[target.reason] ?? '当前不可写入';
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

/** The upload URL for one target. The default target goes without a query, so
 *  the request is the one a firmware from before there were targets knows. */
export function otaUploadUrl(url: string, targetId: string): string {
  return !targetId || targetId === DEFAULT_TARGET ? url : `${url}?target=${encodeURIComponent(targetId)}`;
}

/** arm64 Image header: "ARM\x64" at byte 56. The device checks the same. */
export function isFirmwareImage(head: Uint8Array): boolean {
  return head.length >= 60 && head[56] === 0x41 && head[57] === 0x52 && head[58] === 0x4d && head[59] === 0x64;
}

/** How much of a file `imageFormatOk` wants to see. */
export const IMAGE_HEAD_BYTES = 512;

function startsWith(head: Uint8Array, at: number, bytes: readonly number[]): boolean {
  return head.length >= at + bytes.length && bytes.every((b, i) => head[at + i] === b);
}

/** The magic the device checks for a target, on the first bytes of the file.
 *  Failing here saves the whole transfer; passing proves nothing more than
 *  that the file is the right kind of thing. */
export function imageFormatOk(format: TargetFormat, head: Uint8Array): boolean {
  switch (format) {
    case 'arm64': return isFirmwareImage(head);
    case 'fit': return startsWith(head, 0, [0xd0, 0x0d, 0xfe, 0xed]);
    case 'bootctrl': return startsWith(head, 0, [0x4b, 0x37, 0x41, 0x42, 0x43, 0x54, 0x52, 0x4c]); // "K7ABCTRL"
    case 'fat': return startsWith(head, 510, [0x55, 0xaa]);
    default: return head.length > 0;
  }
}

const FORMAT_HINTS: Record<TargetFormat, string> = {
  arm64: '这不是 NuttX 固件镜像（缺少 arm64 Image 头）。请选择构建产出的 nuttx.bin。',
  fit: '这不是 FIT 镜像（开头不是 d00dfeed）。',
  bootctrl: '这不是 bootctrl 记录（开头不是 K7ABCTRL）。',
  fat: '这不是 FAT 文件系统镜像（首扇区缺少 55AA 签名）。',
  raw: '文件是空的。',
};

export function formatHint(format: TargetFormat): string {
  return FORMAT_HINTS[format];
}

const FORMAT_NAMES: Record<TargetFormat, string> = {
  arm64: 'arm64 Image（nuttx.bin）', fit: 'FIT 镜像（.itb / .img）', bootctrl: 'bootctrl 记录', fat: 'FAT 文件系统镜像', raw: '原始镜像，设备不检查内容',
};

export function formatName(format: TargetFormat): string {
  return FORMAT_NAMES[format];
}

/** "9f86d081…0f00a08" for a place too narrow for 64 digits. */
export function shortDigest(hex: string): string {
  return hex.length > 20 ? `${hex.slice(0, 8)}…${hex.slice(-8)}` : hex;
}

/** The payload of update.apply. `advanced` and `force` are only ever sent as
 *  `true`: leaving them out is how a request says no. */
export function applyRequest(target: UpdateTarget, sha256: string, force: boolean): Record<string, unknown> {
  const req: Record<string, unknown> = { sha256, target: target.id };
  if (target.advanced) req.advanced = true;
  if (force) req.force = true;
  return req;
}

/* ---- advanced targets gate ---- */

const UNLOCK_FALLBACK = '我已了解风险';

/** What the owner has to type before the advanced targets are shown: the
 *  device's name, so that it is this device they are thinking of. */
export function unlockPhrase(deviceName: string | null | undefined): string {
  return (deviceName ?? '').trim() || UNLOCK_FALLBACK;
}

export function unlockMatches(input: string | null, phrase: string): boolean {
  return input !== null && phrase !== '' && input.trim() === phrase;
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
  415: '设备认为这个文件不是所选目标需要的镜像，已丢弃',
  422: '设备收到的数据与本机校验值不一致，已丢弃，请重试',
  507: '设备存储空间不足',
};

/** The device's own word says more than the status it came with. */
const UPLOAD_WORDS: Record<string, string> = {
  ETARGET: '此固件不认识所选的更新目标',
  EBLOCKED: '这个目标不能从面板写入',
  ETOOLARGE: '文件比目标分区大',
  ENOSPACE: '设备 /data 剩余空间不足，放不下这个文件',
};

export function uploadErrorText(httpStatus: number, reply: UploadReply): string {
  if (reply.error === 'ENOTJSON' && httpStatus === 200) return UPLOAD_ERRORS[404]!;
  return UPLOAD_WORDS[reply.error] ?? UPLOAD_ERRORS[httpStatus] ?? `上传失败（HTTP ${httpStatus}${reply.error ? ` ${reply.error}` : ''}）`;
}

const APPLY_REASONS: Record<string, string> = {
  digest: '暂存的文件与确认时的校验值不一致，已丢弃',
  verify: '写入后从存储回读的内容不一致',
  'too-large': '文件比目标分区大',
  format: '暂存的文件不是这个目标需要的镜像',
  io: '读写存储失败',
  'staged-file': '读不到已上传的文件，请重新上传',
  memory: '设备内存不足，无法开始写入',
};

export function applyErrorText(apply: UpdateApply): string {
  const text = APPLY_REASONS[apply.reason] ?? '写入失败';
  return apply.error ? `${text}（错误码 ${apply.error}）` : text;
}

/** Refusals of update.apply that ask something particular of the owner.
 *  ENOTFOUND is not here on purpose: that one means a firmware without the
 *  topic, and the caller says so. */
const APPLY_REFUSALS: Record<string, string> = {
  EADVANCED: '这是高级目标，需要先在「高级选项」中确认风险',
  ERUNNING: '正在运行的固件就在这个分区里，设备拒绝覆盖它',
  EMOUNTED: '这个分区的文件系统正在使用中，需要确认强制写入',
  EBLOCKED: '这个目标不能从面板写入',
  ENOTIMAGE: '已上传的文件不是这个目标需要的镜像，请重新选择文件',
  ETOOLARGE: '已上传的文件比目标分区大',
  ENOTCONFIGURED: '设备上没有已上传的文件，请重新上传',
  EUNAVAILABLE: '设备不是由 N-Boot 启动的，无法确定写入位置',
  EBUSY: '设备正在处理另一次上传或写入，请稍后再试',
};

/** Text for a refused update.apply, or '' when the code is not one of its own. */
export function applyRefusalText(e: unknown): string {
  return isRecord(e) && typeof e.code === 'string' ? APPLY_REFUSALS[e.code] ?? '' : '';
}

export function isMountedRefusal(e: unknown): boolean {
  return isRecord(e) && e.code === 'EMOUNTED';
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
