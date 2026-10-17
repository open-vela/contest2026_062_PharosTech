/* Resumable upload of one model file to PUT /models/upload (pure: the network,
 * the clock and the file come in through `UploadIo`, so the whole protocol is
 * testable without a browser or a device).
 *
 * The contract, as ny_web_models.c implements it:
 *   - the file goes up in pieces of at most 8 MiB, one after the other, each
 *     with `Content-Range: bytes s-e/total` and the SHA-256 of the WHOLE file;
 *   - a piece must start exactly where the device stands. 409 EOFFSET carries
 *     that offset, and so does every error that kept part of a piece;
 *   - GET says the same for a file picked again after a reload;
 *   - when the last byte is in, the device hashes the file and answers
 *     `complete: true`, or 422 EDIGEST after throwing the part away;
 *   - a part that is complete but was never put in place (the connection died
 *     while the device was hashing) is finished by a piece-less request,
 *     `Content-Range: bytes * / total` without the spaces.
 *
 * A link that drops is normal here: about two megabytes a second means minutes
 * per model. So nothing but a refusal ends an upload by itself; everything
 * else is retried with a growing pause, asking the device where it stands
 * first, until the owner pauses or cancels. */

export const PIECE_BYTES = 8 * 1024 * 1024;

function isRecord(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}
function count(v: unknown): number {
  return typeof v === 'number' && Number.isFinite(v) && v > 0 ? Math.floor(v) : 0;
}

/** One answer of /models/upload, GET or PUT. `status` 0 = no answer at all. */
export interface PieceReply {
  status: number;
  /** The body was JSON. A 200 that is not comes from a firmware without the
   *  endpoint: its file server answers every unknown path with the page. */
  json: boolean;
  error: string;
  reason: string;
  received: number;
  total: number;
  sha256: string;
  complete: boolean;
  /** GET only: a finished file of that name is there, and its recorded digest. */
  exists: boolean;
  fileSha256: string;
}

export function parsePieceReply(status: number, body: string): PieceReply {
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
  const text = (v: unknown): string => (typeof v === 'string' ? v : '');
  return {
    status, json,
    error: text(d.error), reason: text(d.reason),
    received: count(d.received), total: count(d.total),
    sha256: text(d.sha256).toLowerCase(), complete: d.complete === true,
    exists: d.exists === true, fileSha256: text(d.fileSha256).toLowerCase(),
  };
}

/** `Content-Range` of a piece, or of the piece-less finishing request. */
export function contentRange(start: number, length: number, total: number): string {
  return length > 0 ? `bytes ${start}-${start + length - 1}/${total}` : `bytes */${total}`;
}

/** Where to go on, from the device's GET answer: only a part of this very
 *  file counts (same size, same digest). */
export function resumeOffset(reply: PieceReply, total: number, sha256: string): number {
  if (reply.status !== 200 || !reply.json) return 0;
  return reply.total === total && reply.sha256 === sha256 ? Math.min(reply.received, total) : 0;
}

/** The finished file on the device already is this file. */
export function alreadyPresent(reply: PieceReply, sha256: string): boolean {
  return reply.status === 200 && reply.json && reply.exists && reply.fileSha256 === sha256;
}

/** Pause before retry number `attempt` (1-based): 1, 2, 4, 8, 15, 30, 30... s. */
export function backoffMs(attempt: number): number {
  const steps = [1000, 2000, 4000, 8000, 15000, 30000];
  return steps[Math.min(Math.max(attempt, 1), steps.length) - 1]!;
}

export type Verdict = 'advance' | 'resync' | 'retry' | 'fatal';

/** What one PUT answer means for the loop. `resync`: the answer itself says
 *  where the device stands. `retry`: nothing useful came back, ask with GET
 *  after a pause. */
export function verdictOf(reply: PieceReply): Verdict {
  if (reply.status === 200) return reply.json ? 'advance' : 'fatal';
  if (reply.status === 409) return reply.error === 'EOFFSET' ? 'resync' : 'retry';
  // A stalled or broken piece: the device kept what it had and says how much.
  if (reply.status === 0 || reply.status === 408 || reply.status === 429 || (reply.status >= 500 && reply.status !== 507)) return 'retry';
  return 'fatal';
}

const REPLY_WORDS: Record<string, string> = {
  EAUTH: '登录凭据已失效，请重新用密码登录后再试',
  EPATH: '设备不接受这个文件名',
  ERANGE: '设备不接受这个分片（Content-Range 无效）',
  ECHUNK: '分片超过设备允许的 8 MB',
  EDIGEST: '设备收到的数据与本机 SHA-256 不一致，已丢弃。请重新上传',
  ENOSPACE: '设备 /data 剩余空间不足（设备始终保留 64 MB 给数据库和日志）',
  EPATHTYPE: '设备上已有同名的目录，或目录位置被文件占用',
  EMETHOD: '此固件不支持模型上传',
  ENOTFOUND: '此固件不支持模型上传',
};

const TOO_LARGE: Record<string, string> = {
  'fat32-file-limit': '单个文件不能超过 4 GB（FAT32 的单文件上限）',
  'offset-width': '此固件的文件偏移是 32 位，单个文件不能超过 2 GB',
};

/** Text for an answer that ended an upload. */
export function replyErrorText(reply: PieceReply): string {
  if (reply.status === 200 && !reply.json) return '此固件不支持模型上传';
  if (reply.error === 'ETOOLARGE') return TOO_LARGE[reply.reason] ?? '文件太大，设备无法存放';
  if (REPLY_WORDS[reply.error]) return REPLY_WORDS[reply.error]!;
  if (reply.status === 401) return REPLY_WORDS.EAUTH!;
  if (reply.status === 404 || reply.status === 405) return '此固件不支持模型上传';
  if (reply.status === 507) return REPLY_WORDS.ENOSPACE!;
  return `上传失败（HTTP ${reply.status}${reply.error ? ` ${reply.error}` : ''}）`;
}

export interface UploadIo {
  /** GET /models/upload?path= */
  getState(path: string): Promise<PieceReply>;
  /** PUT one piece (`piece` null = the finishing request). `onSent` reports
   *  the bytes of this piece that have left the browser. Never rejects: a
   *  transport failure, a timeout and an abort are all `status: 0`. */
  putPiece(path: string, piece: Blob | null, start: number, total: number, sha256: string, onSent: (bytes: number) => void): Promise<PieceReply>;
  /** Resolves after `ms`, or early when the upload is stopped. */
  sleep(ms: number): Promise<void>;
}

export interface UploadSource {
  size: number;
  slice(start: number, end: number): Blob;
}

export interface UploadProgress {
  /** Bytes the device has, plus what of the current piece is on its way. */
  sent: number;
  /** Bytes the device has confirmed. */
  confirmed: number;
  /** 'finishing' = every byte is there, the device is hashing the file. */
  phase: 'checking' | 'sending' | 'finishing' | 'waiting';
  /** Consecutive failures so far, and the text of the last one. */
  attempt: number;
  note: string;
}

export type UploadOutcome =
  | { kind: 'done'; sha256: string; skipped: boolean }
  | { kind: 'stopped'; confirmed: number }
  | { kind: 'failed'; text: string; reply: PieceReply | null; confirmed: number };

export interface UploadOptions {
  path: string;
  source: UploadSource;
  sha256: string;
  io: UploadIo;
  /** Read before every step; true ends the loop with `stopped`. */
  stopped: () => boolean;
  onProgress: (p: UploadProgress) => void;
  pieceBytes?: number;
  /** Consecutive failures after which the upload gives up (the owner can
   *  still resume it by hand). 0 = never. */
  maxAttempts?: number;
}

const RETRY_NOTES: Record<string, string> = {
  EBUSY: '设备正在处理另一个文件，稍后重试',
  ETIMEDOUT: '传输停滞，正在重试',
};

export async function uploadFile(o: UploadOptions): Promise<UploadOutcome> {
  const total = o.source.size;
  const piece = Math.max(1, Math.min(o.pieceBytes ?? PIECE_BYTES, PIECE_BYTES));
  const maxAttempts = o.maxAttempts ?? 0;
  let confirmed = 0;
  let attempt = 0;
  let note = '';
  /* Set when the offset is not known to be the device's: before the first
   * piece, and after anything that came back without one. */
  let ask = true;

  const report = (phase: UploadProgress['phase'], sent: number): void => o.onProgress({ sent, confirmed, phase, attempt, note });

  for (;;) {
    if (o.stopped()) return { kind: 'stopped', confirmed };

    if (ask) {
      report('checking', confirmed);
      const state = await o.io.getState(o.path);
      if (o.stopped()) return { kind: 'stopped', confirmed };
      if (state.status === 200 && state.json) {
        if (alreadyPresent(state, o.sha256)) return { kind: 'done', sha256: o.sha256, skipped: true };
        confirmed = resumeOffset(state, total, o.sha256);
        ask = false;
      } else if (verdictOf(state) === 'retry') {
        attempt++;
        note = state.status === 0 ? '连接中断，正在重试' : RETRY_NOTES[state.error] ?? `设备暂时无法应答（HTTP ${state.status}），正在重试`;
        if (maxAttempts > 0 && attempt >= maxAttempts) return { kind: 'failed', text: `多次重试仍未成功：${note}`, reply: state, confirmed };
        report('waiting', confirmed);
        await o.io.sleep(backoffMs(attempt));
        continue;
      } else {
        return { kind: 'failed', text: replyErrorText(state), reply: state, confirmed };
      }
    }

    const start = confirmed;
    const length = Math.min(piece, total - start);
    const last = start + length >= total;
    report(length > 0 ? 'sending' : 'finishing', start);
    const reply = await o.io.putPiece(o.path, length > 0 ? o.source.slice(start, start + length) : null, start, total, o.sha256, (bytes) => {
      const sent = Math.min(bytes, length);
      report(last && sent >= length ? 'finishing' : 'sending', start + sent);
    });
    if (o.stopped()) return { kind: 'stopped', confirmed };

    switch (verdictOf(reply)) {
      case 'advance':
        if (reply.complete) {
          // Accepted against the header, so anything else is unexplained: do not call it done.
          if (reply.sha256 !== o.sha256) return { kind: 'failed', text: '设备回报的校验值与本机不一致', reply, confirmed };
          confirmed = total;
          report('finishing', total);
          return { kind: 'done', sha256: reply.sha256, skipped: false };
        }
        if (reply.received <= start || reply.received > total) {
          // A 200 that did not move: never loop on it without asking.
          ask = true;
          attempt++;
          note = '设备没有确认这个分片，正在重新核对进度';
          if (maxAttempts > 0 && attempt >= maxAttempts) return { kind: 'failed', text: `多次重试仍未成功：${note}`, reply, confirmed };
          await o.io.sleep(backoffMs(attempt));
          break;
        }
        confirmed = reply.received;
        attempt = 0;
        note = '';
        break;
      case 'resync':
        /* The device says where it stands. Believing it is only safe when it
         * is a part of this file, which EOFFSET with our total implies: the
         * device compared total and digest before it answered. */
        confirmed = Math.min(reply.received, total);
        attempt++;
        if (maxAttempts > 0 && attempt >= maxAttempts) return { kind: 'failed', text: '设备与本机的进度始终对不上', reply, confirmed };
        // One correction is the protocol working; several in a row is not, and must not spin.
        if (attempt > 2) await o.io.sleep(backoffMs(attempt - 2));
        break;
      case 'retry':
        attempt++;
        note = reply.status === 0 ? '连接中断，正在重试' : RETRY_NOTES[reply.error] ?? `设备暂时无法应答（HTTP ${reply.status}），正在重试`;
        if (maxAttempts > 0 && attempt >= maxAttempts) return { kind: 'failed', text: `多次重试仍未成功：${note}`, reply, confirmed };
        // What was kept of a broken piece is only known for sure from a GET.
        ask = true;
        report('waiting', confirmed);
        await o.io.sleep(backoffMs(attempt));
        break;
      default:
        return { kind: 'failed', text: replyErrorText(reply), reply, confirmed: reply.error === 'EDIGEST' ? 0 : confirmed };
    }
  }
}

/* ---- hashing ---- */

export interface Hasher {
  update(data: Uint8Array): unknown;
  hex(): string;
}

export const HASH_STEP = 2 * 1024 * 1024;

/** SHA-256 of a file read in slices: the file is never in memory as a whole,
 *  and every slice is its own await, so the page keeps painting. Returns null
 *  when `stopped()` turned true on the way. */
export async function hashSource(
  source: UploadSource, hasher: Hasher, read: (blob: Blob) => Promise<ArrayBuffer>,
  onProgress: (done: number) => void, stopped: () => boolean, step = HASH_STEP,
): Promise<string | null> {
  for (let at = 0; at < source.size; at += step) {
    if (stopped()) return null;
    const end = Math.min(source.size, at + step);
    hasher.update(new Uint8Array(await read(source.slice(at, end))));
    onProgress(end);
  }
  return stopped() ? null : hasher.hex();
}

/** Key under which a digest is remembered, so that picking the same file again
 *  after a reload does not hash a gigabyte a second time. */
export function digestCacheKey(file: { name: string; size: number; lastModified?: number }): string {
  return `${file.name}|${file.size}|${file.lastModified ?? 0}`;
}
