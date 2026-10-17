/* The compute domain and the on-device model (pure): the compute.status and
 * agent.config.ondevice.* payloads, the wording of their states, transfer
 * progress, and the retry rule for a device that answers EBUSY.
 *
 * The compute domain is the Linux side of an AMP firmware: it holds the NPU
 * and runs the local LLM. The control domain (openvela) copies the model from
 * flash into the compute domain's RAM ("provisioning"), the compute domain
 * loads it, and the agent's model router then sees it as one more backend whose
 * host is `nyabula.local`. A firmware without a compute domain answers
 * ENOTFOUND; that is a normal firmware, not a fault.
 *
 * No imports: the tests load this file with plain node. */

function isRecord(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}
function str(v: unknown): string {
  return typeof v === 'string' ? v : '';
}
function count(v: unknown): number {
  return typeof v === 'number' && Number.isFinite(v) && v > 0 ? v : 0;
}

/* ---- compute.status ---- */

export type LlmState = 'unloaded' | 'provisioning' | 'loading' | 'ready' | 'busy' | 'error' | 'unknown';

const LLM_STATES: readonly string[] = ['unloaded', 'provisioning', 'loading', 'ready', 'busy', 'error'];

export interface ComputeBlob {
  /** A file is being copied into the compute domain right now. */
  active: boolean;
  /** The control domain is hashing the file before it offers it. */
  hashing: boolean;
  name: string;
  offset: number;
  size: number;
  bytesPerSec: number;
}

export interface ComputeLlm {
  state: LlmState;
  model: string;
  /** Of the last finished run; all 0 before the first one. */
  promptTokens: number;
  completionTokens: number;
  prefillMs: number;
  tokensPerSec: number;
  lastError: string;
}

export interface ComputeStatus {
  running: boolean;
  linked: boolean;
  /** Changes every time the compute domain restarts. */
  generation: number;
  capabilities: string[];
  capabilityMask: number;
  blob: ComputeBlob;
  llm: ComputeLlm;
  lastError: string;
  generationChanges: number;
  droppedFrames: number;
}

/** Bits of capabilityMask (ny_compute.h), in the order they are shown. */
const CAPABILITY_BITS: readonly (readonly [number, string])[] = [[1, 'health'], [2, 'llm'], [4, 'blob'], [8, 'chat']];

const CAPABILITY_LABELS: Record<string, string> = {
  health: '心跳',
  llm: '语言模型',
  blob: '文件搬运',
  chat: '对话接口',
};

/** "语言模型" for "llm"; an unknown name is shown as it came. */
export function capabilityLabel(name: string): string {
  return CAPABILITY_LABELS[name] ?? name;
}

export function parseComputeStatus(raw: unknown): ComputeStatus {
  const d = isRecord(raw) ? raw : {};
  const b = isRecord(d.blob) ? d.blob : {};
  const l = isRecord(d.llm) ? d.llm : {};
  const mask = count(d.capabilityMask);
  /* The list is the device's wording of the mask, and an older build words
   * fewer bits than it sets: take both, known names first. */
  const named = Array.isArray(d.capabilities) ? d.capabilities.filter((c): c is string => typeof c === 'string' && c.length > 0 && c.length <= 24) : [];
  const capabilities: string[] = [];
  for (const [bit, name] of CAPABILITY_BITS) {
    if (named.includes(name) || (Math.floor(mask / bit) % 2) === 1) capabilities.push(name);
  }
  for (const name of named) {
    if (!capabilities.includes(name) && capabilities.length < 16) capabilities.push(name);
  }
  const size = count(b.size);
  const state = str(l.state);
  return {
    running: d.running === true,
    linked: d.linked === true,
    generation: count(d.generation),
    capabilities,
    capabilityMask: mask,
    blob: {
      active: b.active === true,
      hashing: b.hashing === true,
      name: str(b.name),
      offset: size > 0 ? Math.min(count(b.offset), size) : count(b.offset),
      size,
      bytesPerSec: count(b.bytesPerSec),
    },
    llm: {
      state: LLM_STATES.includes(state) ? (state as LlmState) : 'unknown',
      model: str(l.model),
      promptTokens: count(l.promptTokens),
      completionTokens: count(l.completionTokens),
      prefillMs: count(l.prefillMs),
      tokensPerSec: count(l.tokensPerSec),
      lastError: str(l.lastError),
    },
    lastError: str(d.lastError),
    generationChanges: count(d.generationChanges),
    droppedFrames: count(d.droppedFrames),
  };
}

/* ---- agent.config.ondevice.* ---- */

export interface OnDeviceConfig {
  /** false: the router slot holds an endpoint the owner typed in. */
  available: boolean;
  enabled: boolean;
  /** Zero-based router slot. */
  slot: number;
  priority: number;
  model: string;
  failures: number;
  calls: number;
  /** Smoothed by the router; 0 before the first call. */
  latencyMs: number;
}

export function parseOnDevice(raw: unknown): OnDeviceConfig {
  const d = isRecord(raw) ? raw : {};
  const available = d.available !== false;
  return {
    available,
    enabled: available && d.enabled === true,
    slot: Math.floor(count(d.slot)),
    priority: Math.min(100, Math.floor(count(d.priority))),
    model: str(d.model),
    failures: Math.floor(count(d.failures)),
    calls: Math.floor(count(d.calls)),
    latencyMs: Math.round(count(d.latencyMs)),
  };
}

/** The host the router stores for the on-device backend. */
export const LOCAL_BACKEND_HOST = 'nyabula.local';

export function isLocalBackend(host: unknown): boolean {
  return typeof host === 'string' && host.trim().toLowerCase() === LOCAL_BACKEND_HOST;
}

/* ---- model files ---- */

/** What compute.llm.load needs under models.list; the load fails before it
 *  moves a byte when either is missing. */
export const LLM_REQUIRED_FILES: readonly { path: string; label: string }[] = [
  { path: 'llm/model.rkllm', label: '模型权重 model.rkllm' },
  { path: 'llm/tokenizer.json', label: '分词器 tokenizer.json' },
];

/** The required files that are not there as finished files. `items` is the
 *  `items` of a parsed models.list (an unfinished upload does not count). */
export function missingLlmFiles(items: readonly { path: string; partial: boolean }[]): { path: string; label: string }[] {
  return LLM_REQUIRED_FILES.filter((f) => !items.some((i) => i.path === f.path && !i.partial));
}

/* ---- progress ---- */

/** 0..100, whole; 0 when the size is not known. */
export function blobPercent(blob: { offset: number; size: number }): number {
  if (!(blob.size > 0) || !(blob.offset > 0)) return 0;
  return Math.max(0, Math.min(100, Math.floor((blob.offset / blob.size) * 100)));
}

/** "12.5 MB/s" (binary units, as the rest of the panel); '' when nothing moves. */
export function fmtRate(bytesPerSec: number): string {
  if (!Number.isFinite(bytesPerSec) || bytesPerSec <= 0) return '';
  const units = ['B', 'KB', 'MB', 'GB'];
  let value = bytesPerSec;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  const text = unit === 0 || value >= 100 ? String(Math.round(value)) : value.toFixed(1).replace(/\.0$/, '');
  return `${text} ${units[unit]}/s`;
}

/** Seconds until the transfer ends at the present rate; -1 when it cannot be said. */
export function blobEtaSeconds(blob: { offset: number; size: number; bytesPerSec: number }): number {
  if (!(blob.size > 0) || !(blob.bytesPerSec > 0)) return -1;
  return Math.max(0, Math.ceil((blob.size - Math.min(blob.offset, blob.size)) / blob.bytesPerSec));
}

/** "约 45 秒" / "约 1 分 20 秒"; '' when it cannot be said. */
export function fmtEta(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return '';
  const s = Math.round(seconds);
  if (s < 1) return '即将完成';
  if (s < 60) return `约 ${s} 秒`;
  if (s < 3600) return `约 ${Math.floor(s / 60)} 分 ${s % 60} 秒`;
  return `约 ${Math.floor(s / 3600)} 小时 ${Math.floor((s % 3600) / 60)} 分`;
}

/** "正在搬运模型 42% · 12.5 MB/s · 剩余约 40 秒" and its shorter forms. */
export function transferText(blob: ComputeBlob, lead: string): string {
  const parts = [blob.size > 0 ? `${lead} ${blobPercent(blob)}%` : lead];
  const rate = fmtRate(blob.bytesPerSec);
  if (rate) parts.push(rate);
  const eta = fmtEta(blobEtaSeconds(blob));
  if (eta) parts.push(eta === '即将完成' ? eta : `剩余${eta}`);
  return parts.join(' · ');
}

export type Tone = 'ok' | 'warn' | 'err' | 'info';

/** The one-line state of the local model. */
export function llmStatusText(status: ComputeStatus): string {
  if (!status.linked) return status.running ? '计算域正在启动，尚未连上' : '计算域没有运行';
  switch (status.llm.state) {
    case 'unloaded': return '未加载';
    case 'provisioning': return status.blob.hashing && !status.blob.active ? '正在核对模型文件…' : transferText(status.blob, '正在搬运模型');
    case 'loading': return '正在加载';
    case 'ready': return '就绪';
    case 'busy': return '推理中';
    case 'error': return `出错: ${status.llm.lastError || status.lastError || '原因未知'}`;
    default: return '状态未知（固件比面板新）';
  }
}

export function llmStatusTone(status: ComputeStatus): Tone {
  if (!status.linked) return 'warn';
  switch (status.llm.state) {
    case 'ready': case 'busy': return 'ok';
    case 'error': return 'err';
    case 'provisioning': case 'loading': return 'warn';
    default: return 'info';
  }
}

/** A state that changes by itself within seconds. */
export function isTransient(state: LlmState): boolean {
  return state === 'provisioning' || state === 'loading' || state === 'busy';
}

export const POLL_FAST_MS = 1000;
export const POLL_SLOW_MS = 10000;

/** How long to wait before asking compute.status again. `blobActive` keeps the
 *  fast pace for a transfer nobody asked the LLM for (an upload being pulled). */
export function pollIntervalMs(state: LlmState, blobActive = false): number {
  return isTransient(state) || blobActive ? POLL_FAST_MS : POLL_SLOW_MS;
}

/** "提示 128 tokens · 回复 64 tokens · 预填充 850 ms · 9.6 tokens/s"; '' before the first run. */
export function lastRunText(llm: ComputeLlm): string {
  if (llm.promptTokens <= 0 && llm.completionTokens <= 0) return '';
  const parts = [`提示 ${llm.promptTokens} tokens`, `回复 ${llm.completionTokens} tokens`];
  if (llm.prefillMs > 0) parts.push(`预填充 ${Math.round(llm.prefillMs)} ms`);
  if (llm.tokensPerSec > 0) parts.push(`${llm.tokensPerSec.toFixed(1).replace(/\.0$/, '')} tokens/s`);
  return parts.join(' · ');
}

/* ---- errors and retry ---- */

/** The NyaLink error code of a rejected request, or ''. */
export function errorCode(e: unknown): string {
  return isRecord(e) && typeof e.code === 'string' ? e.code : '';
}

export const BUSY_RETRIES = 5;
export const BUSY_DELAY_MS = 2000;

export interface RetryOptions {
  /** Retries after the first attempt. */
  retries?: number;
  delayMs?: number;
  /** Called before each wait with the number of the retry about to be made (1-based). */
  onRetry?: (attempt: number, retries: number) => void;
  /** Return true to give up early (the view went away, the session changed). */
  cancelled?: () => boolean;
  sleep?: (ms: number) => Promise<void>;
}

const defaultSleep = (ms: number): Promise<void> => new Promise((resolve) => { setTimeout(resolve, ms); });

/** Run `fn`; while it fails with EBUSY (a run is active, or the agent is still
 *  starting) wait and run it again, up to `retries` more times. Any other
 *  failure, and the last EBUSY, is thrown as it came. */
export async function retryOnBusy<T>(fn: () => Promise<T>, opts: RetryOptions = {}): Promise<T> {
  const retries = opts.retries ?? BUSY_RETRIES;
  const delayMs = opts.delayMs ?? BUSY_DELAY_MS;
  const sleep = opts.sleep ?? defaultSleep;
  for (let attempt = 0; ; attempt++) {
    try {
      return await fn();
    } catch (e) {
      if (errorCode(e) !== 'EBUSY' || attempt >= retries || opts.cancelled?.()) throw e;
      opts.onRetry?.(attempt + 1, retries);
      await sleep(delayMs);
      if (opts.cancelled?.()) throw e;
    }
  }
}

/** What to tell the owner when an on-device request failed. */
export function onDeviceErrorText(e: unknown): string {
  switch (errorCode(e)) {
    case 'EBUSY': return '智能体正忙（对话进行中或仍在启动），请稍后再试';
    case 'EEXIST': return '路由槽位已被自定义后端占用，请先在下方「多模型路由」里移走或删除它';
    case 'EACCES': case 'EPERM': return '需要主人权限';
    case 'ENOENT': return '设备上缺少模型文件，请先到「模型」页上传';
    case 'ETIMEDOUT': return '设备没有及时回应，请稍后重试';
    default: {
      const message = isRecord(e) && typeof e.message === 'string' ? e.message : e instanceof Error ? e.message : String(e);
      return message || '请求失败';
    }
  }
}
