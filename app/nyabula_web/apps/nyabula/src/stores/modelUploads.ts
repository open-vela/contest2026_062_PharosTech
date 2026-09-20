/* Model upload queue. It lives in a store and not in the page, because one
 * language model takes about seven minutes over the device's WiFi and the
 * owner will look at other pages meanwhile: leaving the models page must not
 * end the transfer.
 *
 * One file at a time (the device takes one upload at a time anyway):
 *   hash here in slices -> GET where the device stands -> PUT pieces of 8 MiB
 *   -> the device hashes the whole file and puts it in place.
 * The protocol itself is lib/modelUpload.ts; this is the browser around it:
 * XMLHttpRequest with a stall watchdog, the queue, pause / resume / cancel,
 * speed and time left. */
import { computed, reactive, ref, watch } from 'vue';
import { defineStore } from 'pinia';
import { useSessionStore } from './session';
import { createSha256 } from '../lib/sha256';
import {
  destinationFor, modelsEndpoint, modelsUploadUrl, parseModelsStatus, pushSample, sizeRefusal, transferRate,
  type ModelKind, type ModelsList, type RateSample,
} from '../lib/deviceModels';
import { PIECE_BYTES, contentRange, digestCacheKey, hashSource, parsePieceReply, uploadFile, type PieceReply, type UploadIo } from '../lib/modelUpload';

export type TaskPhase = 'queued' | 'hashing' | 'checking' | 'uploading' | 'finishing' | 'waiting' | 'paused' | 'done' | 'failed';

export interface UploadTask {
  id: number;
  kind: ModelKind;
  /** Device path, `<kind>/<name>`. */
  path: string;
  /** Name of the picked file, which may differ from the one it is stored under. */
  fileName: string;
  renamed: boolean;
  size: number;
  phase: TaskPhase;
  sha256: string;
  /** Bytes hashed here, bytes on their way, bytes the device confirmed. */
  hashed: number;
  sent: number;
  confirmed: number;
  /** Bytes the device has hashed of the finished file (models.status). */
  deviceHashed: number;
  /** Bytes per second, 0 when unknown; seconds left, -1 when unknown. */
  rate: number;
  eta: number;
  /** Why it is waiting (retry), and why it failed. */
  note: string;
  error: string;
  /** The device already had exactly this file. */
  skipped: boolean;
}

/** No upload progress for this long = the link is gone, whatever the socket says. */
const STALL_MS = 75000;
/** An ordinary piece is answered at once; the last one after the device has
 *  hashed the whole file from storage, which is minutes for a large model. */
const REPLY_MS = 90000;
const FINISH_MS = 15 * 60 * 1000;
const STATE_MS = 20000;
const RATE_TICK_MS = 500;
const STATUS_POLL_MS = 2000;
const DIGEST_CACHE = 'nya.modelDigests';
const DIGEST_CACHE_MAX = 32;

function readDigestCache(): Record<string, string> {
  try {
    const parsed: unknown = JSON.parse(localStorage.getItem(DIGEST_CACHE) ?? '{}');
    return typeof parsed === 'object' && parsed !== null && !Array.isArray(parsed) ? (parsed as Record<string, string>) : {};
  } catch {
    return {};
  }
}

function rememberDigest(key: string, sha256: string): void {
  try {
    const cache = readDigestCache();
    delete cache[key];
    cache[key] = sha256;
    const keys = Object.keys(cache);
    for (const old of keys.slice(0, Math.max(0, keys.length - DIGEST_CACHE_MAX))) delete cache[old];
    localStorage.setItem(DIGEST_CACHE, JSON.stringify(cache));
  } catch {
    /* private mode: hash again next time */
  }
}

export const useModelUploadsStore = defineStore('modelUploads', () => {
  const session = useSessionStore();
  const tasks = ref<UploadTask[]>([]);
  /** Bumped whenever a file reached (or left) the device: the page lists again. */
  const changed = ref(0);

  /* Not reactive on purpose: a File has nothing to observe. */
  const files = new Map<number, File>();
  let nextId = 1;
  let pumping = false;
  /* The run in progress: its stop flag, the request to abort and the sleeper to wake. */
  let current: { id: number; stopped: boolean; abort: (() => void) | null; wake: (() => void) | null } | null = null;

  const endpoint = computed(() => modelsEndpoint(session.deviceKey, location));
  const active = computed(() => tasks.value.find((t) => ['hashing', 'checking', 'uploading', 'finishing', 'waiting'].includes(t.phase)) ?? null);
  const busy = computed(() => active.value !== null || tasks.value.some((t) => t.phase === 'queued'));

  function token(): string | null {
    try {
      return endpoint.value ? localStorage.getItem(endpoint.value.tokenSlot) : null;
    } catch {
      return null;
    }
  }

  /** Why nothing can be uploaded from this page at all, or ''. */
  const blocked = computed(() => {
    if (!endpoint.value) return '模型只能从设备自己提供的页面上传。请在浏览器里直接打开设备地址（http://设备 IP/）再来这里。';
    if (!session.isOwner) return '只有设备主人可以上传模型。';
    return '';
  });

  /* ---- transport ---- */

  function request(method: 'GET' | 'PUT', path: string, body: Blob | null, headers: Record<string, string>, onSent: ((bytes: number) => void) | null, replyMs: number): Promise<PieceReply> {
    return new Promise((resolve) => {
      const ep = endpoint.value;
      const tk = token();
      if (!ep || !tk) return resolve(parsePieceReply(401, '{"error":"EAUTH"}'));
      const x = new XMLHttpRequest();
      let timer: ReturnType<typeof setTimeout> | null = null;
      let settled = false;
      const arm = (ms: number): void => {
        if (timer) clearTimeout(timer);
        timer = setTimeout(() => x.abort(), ms);
      };
      const done = (): void => {
        if (settled) return;
        settled = true;
        if (timer) clearTimeout(timer);
        if (current?.abort === abort) current.abort = null;
        resolve(parsePieceReply(x.status, x.responseText ?? ''));
      };
      const abort = (): void => x.abort();
      x.open(method, modelsUploadUrl(ep.url, path));
      x.setRequestHeader('Authorization', `Bearer ${tk}`);
      for (const [k, v] of Object.entries(headers)) x.setRequestHeader(k, v);
      if (onSent) {
        x.upload.onprogress = (ev) => {
          arm(STALL_MS);
          onSent(ev.loaded);
        };
        // Everything has left: from here on it is the device's turn.
        x.upload.onload = () => arm(replyMs);
      }
      x.onload = done;
      x.onerror = done;
      x.onabort = done;
      if (current) current.abort = abort;
      // An empty body raises no upload events: the wait for the answer starts now.
      arm(onSent && body && body.size > 0 ? STALL_MS : replyMs);
      x.send(body);
    });
  }

  function makeIo(run: NonNullable<typeof current>): UploadIo {
    return {
      getState: (path) => request('GET', path, null, {}, null, STATE_MS),
      putPiece: (path, piece, start, total, sha256, onSent) => {
        const length = piece?.size ?? 0;
        const last = start + length >= total;
        return request(
          'PUT', path,
          // A zero-length Blob and not null: the device wants `Content-Length: 0` spelled out.
          piece ?? new Blob([]),
          { 'Content-Type': 'application/octet-stream', 'Content-Range': contentRange(start, length, total), 'X-Nya-Sha256': sha256 },
          onSent, last ? FINISH_MS : REPLY_MS,
        );
      },
      sleep: (ms) => new Promise<void>((resolve) => {
        if (run.stopped) return resolve();
        const timer = setTimeout(() => {
          run.wake = null;
          resolve();
        }, ms);
        run.wake = () => {
          clearTimeout(timer);
          run.wake = null;
          resolve();
        };
      }),
    };
  }

  /* ---- queue ---- */

  /** Queue files dropped on one kind. Returns a line per file that was not
   *  taken, for the page to show. */
  function add(kind: ModelKind, picked: File[], list: ModelsList | null): string[] {
    const refused: string[] = [];
    const taken = new Set<string>();
    for (const file of picked) {
      const dest = destinationFor(kind, file.name, taken);
      if (dest.spec) taken.add(dest.spec.name);
      const held = list?.items.find((i) => i.partial && i.path === dest.path && i.total === file.size)?.received ?? 0;
      const why = sizeRefusal(list, file.size, held);
      if (why) {
        refused.push(`${file.name}：${why}`);
        continue;
      }
      // A second pick for the same destination replaces one that has not started or has ended.
      const same = tasks.value.find((t) => t.path === dest.path);
      if (same && ['queued', 'paused', 'done', 'failed'].includes(same.phase)) dismiss(same.id);
      else if (same) {
        refused.push(`${file.name}：「${dest.path}」正在上传，请先取消它`);
        continue;
      }
      const id = nextId++;
      files.set(id, file);
      tasks.value.push(reactive<UploadTask>({
        id, kind, path: dest.path, fileName: file.name, renamed: dest.renamed, size: file.size, phase: 'queued', sha256: '',
        hashed: 0, sent: 0, confirmed: 0, deviceHashed: 0, rate: 0, eta: -1, note: '', error: '', skipped: false,
      }));
    }
    void pump();
    return refused;
  }

  async function pump(): Promise<void> {
    if (pumping) return;
    pumping = true;
    try {
      for (;;) {
        const task = tasks.value.find((t) => t.phase === 'queued');
        if (!task) break;
        await runTask(task);
      }
    } finally {
      pumping = false;
    }
  }

  async function runTask(task: UploadTask): Promise<void> {
    const file = files.get(task.id);
    if (!file) {
      task.phase = 'failed';
      task.error = '文件已不在页面里，请重新选择';
      return;
    }
    const run: NonNullable<typeof current> = { id: task.id, stopped: false, abort: null, wake: null };
    current = run;
    task.error = '';
    task.note = '';
    let samples: RateSample[] = [];
    let lastTick = 0;
    let poll: ReturnType<typeof setInterval> | null = null;
    const stopPoll = (): void => {
      if (poll) clearInterval(poll);
      poll = null;
    };
    try {
      if (!task.sha256) {
        const key = digestCacheKey(file);
        const cached = readDigestCache()[key];
        if (cached && /^[0-9a-f]{64}$/.test(cached)) task.sha256 = cached;
        else {
          task.phase = 'hashing';
          task.hashed = 0;
          const sha = await hashSource(file, createSha256(), (b) => b.arrayBuffer(), (n) => { task.hashed = n; }, () => run.stopped);
          if (sha === null) return;
          task.sha256 = sha;
          rememberDigest(key, sha);
        }
      }
      task.hashed = task.size;

      const outcome = await uploadFile({
        path: task.path, source: file, sha256: task.sha256, io: makeIo(run), pieceBytes: PIECE_BYTES, stopped: () => run.stopped,
        onProgress: (p) => {
          // pause() and cancel() have the last word on what the task is.
          if (run.stopped) return;
          task.phase = p.phase === 'sending' ? 'uploading' : p.phase;
          task.note = p.note;
          task.sent = p.sent;
          task.confirmed = p.confirmed;
          const now = Date.now();
          if (p.phase === 'sending' && now - lastTick >= RATE_TICK_MS) {
            lastTick = now;
            samples = pushSample(samples, now, p.sent);
            task.rate = transferRate(samples);
            task.eta = task.rate > 0 ? (task.size - p.sent) / task.rate : -1;
          }
          if (p.phase === 'waiting') {
            samples = [];
            task.rate = 0;
            task.eta = -1;
          }
          // While the device hashes, the only news there is comes over the socket.
          if (p.phase === 'finishing' && !poll) {
            poll = setInterval(() => {
              session.request('models.status').then((raw) => {
                const st = parseModelsStatus(raw).upload;
                if (st.active && st.path === task.path && st.phase === 'hashing') task.deviceHashed = st.hashed;
              }).catch(() => undefined);
            }, STATUS_POLL_MS);
          } else if (p.phase !== 'finishing') stopPoll();
        },
      });

      if (outcome.kind === 'done') {
        task.phase = 'done';
        task.skipped = outcome.skipped;
        task.sent = task.size;
        task.confirmed = task.size;
        task.rate = 0;
        task.eta = -1;
        files.delete(task.id);
        changed.value++;
      } else if (outcome.kind === 'failed') {
        task.phase = 'failed';
        task.error = outcome.text;
        task.confirmed = outcome.confirmed;
        task.sent = outcome.confirmed;
        task.rate = 0;
        task.eta = -1;
        changed.value++;
      }
      // 'stopped': pause() / cancel() have already said what the task is now.
    } catch (e) {
      if (!run.stopped) {
        task.phase = 'failed';
        task.error = `读取文件失败：${e instanceof Error ? e.message : String(e)}`;
      }
    } finally {
      stopPoll();
      if (current === run) current = null;
    }
  }

  function halt(id: number): void {
    if (current?.id !== id) return;
    current.stopped = true;
    current.abort?.();
    current.wake?.();
  }

  function pause(id: number): void {
    const task = tasks.value.find((t) => t.id === id);
    if (!task || ['done', 'failed', 'paused'].includes(task.phase)) return;
    task.phase = 'paused';
    task.rate = 0;
    task.eta = -1;
    task.note = '';
    halt(id);
  }

  /** Also the way out of `failed`: the device is asked where it stands first. */
  function resume(id: number): void {
    const task = tasks.value.find((t) => t.id === id);
    if (!task || (task.phase !== 'paused' && task.phase !== 'failed') || !files.has(id)) return;
    task.phase = 'queued';
    task.error = '';
    void pump();
  }

  /** Take a task off the list without touching the device. */
  function dismiss(id: number): void {
    halt(id);
    files.delete(id);
    tasks.value = tasks.value.filter((t) => t.id !== id);
  }

  /** Stop, and remove what the device holds of it. The device lets go of the
   *  file a moment after the connection is gone, so EBUSY is tried again. */
  async function cancel(id: number): Promise<void> {
    const task = tasks.value.find((t) => t.id === id);
    if (!task) return;
    const path = task.path;
    const touched = task.phase !== 'queued' && task.phase !== 'hashing' && task.phase !== 'done';
    dismiss(id);
    if (!touched) return;
    for (let i = 0; i < 5; i++) {
      try {
        await session.request('models.delete', { path }, { timeoutMs: 15000 });
        break;
      } catch (e) {
        if (typeof e !== 'object' || e === null || (e as { code?: unknown }).code !== 'EBUSY') break;
        await new Promise((r) => setTimeout(r, 1000));
      }
    }
    changed.value++;
  }

  function clearFinished(): void {
    for (const t of tasks.value.filter((x) => x.phase === 'done')) files.delete(t.id);
    tasks.value = tasks.value.filter((t) => t.phase !== 'done');
  }

  // Another device, or none: these files were meant for the one that is gone.
  watch(() => session.deviceKey, () => {
    for (const t of [...tasks.value]) dismiss(t.id);
  });

  return { tasks, changed, active, busy, blocked, endpoint, token, add, pause, resume, cancel, dismiss, clearFinished };
});
