/* Model files on the device (pure): the catalogue of what the compute domain
 * looks for under /data/models, the models.list / models.status payloads, and
 * the rules that turn a dropped file into a device path.
 *
 * The device knows nothing about models: a kind is a directory and the rest is
 * a file name. Which names matter is knowledge of the panel, kept in one table
 * here. The models themselves are third-party files the owner brings; neither
 * the firmware nor this repository ships one. */

function isRecord(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}
function str(v: unknown): string {
  return typeof v === 'string' ? v : '';
}
function size(v: unknown): number {
  return typeof v === 'number' && Number.isFinite(v) && v > 0 ? v : 0;
}

/* ---- catalogue ---- */

export type ModelKind = 'llm' | 'asr' | 'tts' | 'kws' | 'speaker' | 'face';

export interface ModelFileSpec {
  /** Path below the kind directory, as the compute domain opens it. */
  name: string;
  /** What this file is, in a few words. */
  label: string;
  required: boolean;
  /** Names the same file has where people download it (tested on the lowercased base name). */
  aliases: RegExp[];
  /** Typical size, for the hint only: another build of the model may differ. */
  typicalBytes?: number;
}

export interface ModelKindSpec {
  kind: ModelKind;
  label: string;
  icon: string;
  /** What the model is for, for someone who has never heard its name. */
  purpose: string;
  /** Where it usually comes from. */
  source: string;
  files: ModelFileSpec[];
}

const MB = 1024 * 1024;

/* The ASR and KWS transducers are published as "encoder-epoch-99-avg-1.int8.onnx"
 * and the like; the services open the short names. */
const transducer = (required: boolean): ModelFileSpec[] => [
  { name: 'encoder.onnx', label: '编码器', required, aliases: [/^encoder.*\.onnx$/] },
  { name: 'decoder.onnx', label: '解码器', required, aliases: [/^decoder.*\.onnx$/] },
  { name: 'joiner.onnx', label: '联合网络', required, aliases: [/^joiner.*\.onnx$/] },
  { name: 'tokens.txt', label: '词表', required, aliases: [] },
];

export const MODEL_CATALOGUE: readonly ModelKindSpec[] = [
  {
    kind: 'llm', label: '语言模型（LLM）', icon: 'chat',
    purpose: '离线对话用的大语言模型，在 NPU 上运行。没有它，Nyabot 只能使用云端模型。',
    source: 'MiniCPM5-1B，经 rkllm-toolkit 1.3.0 转成 W4A16 的 .rkllm',
    files: [
      { name: 'model.rkllm', label: '模型权重', required: true, aliases: [/\.rkllm$/], typicalBytes: 835 * MB },
      { name: 'tokenizer/tokenizer.json', label: '分词器', required: false, aliases: [/^tokenizer\.json$/] },
      { name: 'tokenizer/tokenizer_config.json', label: '分词器配置', required: false, aliases: [/^tokenizer_config\.json$/] },
      { name: 'tokenizer/chat_template.jinja', label: '对话模板', required: false, aliases: [/^chat_template\.jinja$/] },
      { name: 'tokenizer/special_tokens_map.json', label: '特殊符号表', required: false, aliases: [/^special_tokens_map\.json$/] },
    ],
  },
  {
    kind: 'asr', label: '语音识别（ASR）', icon: 'mic',
    purpose: '把你说的话转成文字。流式识别，边说边出字。',
    source: 'sherpa-onnx streaming-zipformer-zh-14M（INT8，约 24 MB）',
    files: transducer(true).map((f) => (f.name === 'encoder.onnx' ? { ...f, typicalBytes: 21 * MB } : f)),
  },
  {
    kind: 'tts', label: '语音合成（TTS）', icon: 'volume_up',
    purpose: '把文字读出来，是 Nyabula 的声音。前半段在 CPU 上算，声码器在 NPU 上算。',
    source: 'MeloTTS zh_en：prefix.onnx 与经 rknn-toolkit2 转换的 vocoder.rknn；词典来自 sherpa-onnx 的 vits-melo-tts-zh_en 包',
    files: [
      { name: 'prefix.onnx', label: '声学模型前半', required: true, aliases: [/^prefix.*\.onnx$/], typicalBytes: 106 * MB },
      { name: 'vocoder.rknn', label: '声码器（NPU）', required: true, aliases: [/^vocoder.*\.rknn$/], typicalBytes: 38 * MB },
      { name: 'lexicon.txt', label: '发音词典', required: true, aliases: [], typicalBytes: 7 * MB },
      { name: 'tokens.txt', label: '音素表', required: true, aliases: [] },
      { name: 'dict/jieba.dict.utf8', label: '分词词典', required: false, aliases: [/^jieba\.dict\.utf8$/] },
    ],
  },
  {
    kind: 'kws', label: '唤醒词（KWS）', icon: 'notifications',
    purpose: '一直在听「你好，openvela」的小模型。只有它听到了，后面的识别才会启动。',
    source: 'sherpa-onnx 关键词检出 zipformer（约几 MB）',
    files: [...transducer(true), { name: 'keywords.txt', label: '唤醒词列表', required: false, aliases: [] }],
  },
  {
    kind: 'speaker', label: '声纹识别', icon: 'person',
    purpose: '分辨是谁在说话，让设备只对主人的声音做出某些回应。',
    source: '说话人嵌入模型（ONNX，约 28 MB）',
    files: [{ name: 'model.onnx', label: '说话人嵌入模型', required: true, aliases: [/\.onnx$/], typicalBytes: 28 * MB }],
  },
  {
    kind: 'face', label: '人脸识别', icon: 'face',
    purpose: '认出主人的脸。只在设备上比对，照片不会离开设备。',
    source: 'OpenCV Zoo SFace（face_recognition_sface）',
    files: [{ name: 'sface.onnx', label: '人脸特征模型', required: true, aliases: [/sface.*\.onnx$/] }],
  },
];

export function kindSpec(kind: string): ModelKindSpec | null {
  return MODEL_CATALOGUE.find((k) => k.kind === kind) ?? null;
}

/* ---- names ---- */

/** Limits of the device (ny_web_models.c): it refuses what does not pass. */
export const PATH_MAX = 96;
export const SEGMENT_MAX = 48;
export const LEVELS_MAX = 3;
const RESERVED = ['.part', '.part.json', '.sha256'];

function baseName(name: string): string {
  const cut = Math.max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
  return cut >= 0 ? name.slice(cut + 1) : name;
}

/** One path segment the device accepts, made from whatever the file was called. */
export function safeSegment(name: string): string {
  let s = baseName(name).normalize('NFKD').replace(/[^A-Za-z0-9._-]/g, '_').replace(/_+/g, '_');
  s = s.replace(/^\.+/, '').replace(/\.+$/, '');
  if (s.length > SEGMENT_MAX) {
    // Keep the extension: it is what tells an .onnx from an .rknn later.
    const dot = s.lastIndexOf('.');
    const ext = dot > 0 && s.length - dot <= 12 ? s.slice(dot) : '';
    s = s.slice(0, SEGMENT_MAX - ext.length).replace(/\.+$/, '') + ext;
  }
  const lower = s.toLowerCase();
  if (RESERVED.some((r) => lower.endsWith(r))) s = `${s.slice(0, SEGMENT_MAX - 1)}_`;
  return s || 'file';
}

/** The same test the device applies to `<kind>/<relative name>`. */
export function isModelPath(path: string): boolean {
  if (!path || path.length > PATH_MAX) return false;
  const parts = path.split('/');
  if (parts.length < 2 || parts.length > 1 + LEVELS_MAX || !kindSpec(parts[0]!)) return false;
  if (!parts.every((p) => p.length > 0 && p.length <= SEGMENT_MAX && /^[A-Za-z0-9_-][A-Za-z0-9._-]*$/.test(p) && !p.endsWith('.'))) return false;
  const last = parts[parts.length - 1]!.toLowerCase();
  return !RESERVED.some((r) => last.length > r.length && last.endsWith(r));
}

export interface Destination {
  /** `<kind>/<name>` as sent to the device. */
  path: string;
  /** The catalogue entry it fills; null for a file the catalogue does not know. */
  spec: ModelFileSpec | null;
  /** The dropped file goes under another name than its own. */
  renamed: boolean;
}

/** Where a dropped file goes: the expected name when its own name is that name
 *  or one of its known aliases, else its own name made safe. `taken` holds the
 *  expected names already claimed in this drop, so that two files that match
 *  the same loose alias do not overwrite each other. */
export function destinationFor(kind: ModelKind, fileName: string, taken: ReadonlySet<string> = new Set()): Destination {
  const base = baseName(fileName);
  const lower = base.toLowerCase();
  const files = kindSpec(kind)?.files ?? [];
  const exact = files.find((f) => baseName(f.name).toLowerCase() === lower);
  const spec = exact ?? files.find((f) => !taken.has(f.name) && f.aliases.some((a) => a.test(lower))) ?? null;
  if (spec) return { path: `${kind}/${spec.name}`, spec, renamed: baseName(spec.name) !== base };
  const safe = safeSegment(base);
  return { path: `${kind}/${safe}`, spec: null, renamed: safe !== base };
}

/* ---- models.list ---- */

export interface ModelItem {
  path: string;
  kind: string;
  bytes: number;
  /** Unix milliseconds; 0 when the device has no clock yet. */
  mtime: number;
  /** An upload that has not finished: `bytes` is what is there of it. */
  partial: boolean;
  received: number;
  total: number;
  /** Of the finished file (from its sidecar), or of the file a partial belongs to; '' when unknown. */
  sha256: string;
}

export interface ModelsList {
  root: string;
  free: number;
  volume: number;
  /** Bytes the device keeps free whatever is uploaded. */
  margin: number;
  /** Largest single file the volume can hold. */
  fileLimit: number;
  pieceLimit: number;
  kinds: string[];
  items: ModelItem[];
  /** The listing did not fit one message: some files are not shown. */
  truncated: boolean;
  /** Only in the answer of models.delete. */
  removed: boolean | null;
}

export const DEFAULT_MARGIN = 64 * MB;
export const DEFAULT_FILE_LIMIT = 0xffffffff;
export const DEFAULT_PIECE = 8 * MB;

export function parseModelsList(raw: unknown): ModelsList {
  const d = isRecord(raw) ? raw : {};
  const items: ModelItem[] = [];
  for (const it of Array.isArray(d.items) ? d.items.filter(isRecord) : []) {
    const path = str(it.path);
    if (!path || items.some((x) => x.path === path && x.partial === (it.partial === true))) continue;
    const sha = str(it.sha256).toLowerCase();
    items.push({
      path,
      kind: str(it.kind) || path.split('/')[0]!,
      bytes: size(it.bytes),
      mtime: size(it.mtime),
      partial: it.partial === true,
      received: size(it.received),
      total: size(it.total),
      sha256: /^[0-9a-f]{64}$/.test(sha) ? sha : '',
    });
  }
  return {
    root: str(d.root) || '/data/models',
    free: size(d.free),
    volume: size(d.volume),
    margin: size(d.margin) || DEFAULT_MARGIN,
    fileLimit: size(d.fileLimit) || DEFAULT_FILE_LIMIT,
    pieceLimit: size(d.pieceLimit) || DEFAULT_PIECE,
    kinds: Array.isArray(d.kinds) ? d.kinds.filter((k): k is string => typeof k === 'string') : [],
    items,
    truncated: d.truncated === true,
    removed: typeof d.removed === 'boolean' ? d.removed : null,
  };
}

export type FileState = 'present' | 'partial' | 'missing';

export interface ModelRow {
  path: string;
  name: string;
  spec: ModelFileSpec | null;
  state: FileState;
  /** The finished file, when there is one. */
  file: ModelItem | null;
  /** An unfinished upload of it, when there is one (it can coexist with `file`). */
  part: ModelItem | null;
}

export interface ModelGroup {
  spec: ModelKindSpec;
  rows: ModelRow[];
  /** Every required file is present. */
  ready: boolean;
  missingRequired: number;
  bytes: number;
}

/** The catalogue laid over what the device reports: expected files first, in
 *  catalogue order, then whatever else is in the directory. */
export function groupModels(list: ModelsList | null): ModelGroup[] {
  const items = list?.items ?? [];
  return MODEL_CATALOGUE.map((spec): ModelGroup => {
    const mine = items.filter((i) => i.kind === spec.kind);
    const known = new Set(spec.files.map((f) => `${spec.kind}/${f.name}`));
    const row = (path: string, fileSpec: ModelFileSpec | null): ModelRow => {
      const file = mine.find((i) => i.path === path && !i.partial) ?? null;
      const part = mine.find((i) => i.path === path && i.partial) ?? null;
      return { path, name: path.slice(spec.kind.length + 1), spec: fileSpec, state: file ? 'present' : part ? 'partial' : 'missing', file, part };
    };
    const extras = [...new Set(mine.map((i) => i.path).filter((p) => !known.has(p)))].sort();
    const rows = [...spec.files.map((f) => row(`${spec.kind}/${f.name}`, f)), ...extras.map((p) => row(p, null))];
    const missingRequired = rows.filter((r) => r.spec?.required && r.state !== 'present').length;
    return { spec, rows, ready: missingRequired === 0, missingRequired, bytes: mine.reduce((sum, i) => sum + i.bytes, 0) };
  });
}

/** Why a file of this size cannot be uploaded right now, or ''. `held` is what
 *  the device already has of it (a resumed upload needs only the rest). */
export function sizeRefusal(list: ModelsList | null, bytes: number, held = 0): string {
  if (bytes <= 0) return '文件是空的';
  const limit = list?.fileLimit ?? DEFAULT_FILE_LIMIT;
  if (bytes > limit) return `单个文件不能超过 ${limit >= DEFAULT_FILE_LIMIT ? '4 GB（FAT32 的单文件上限）' : '2 GB（此固件的文件偏移是 32 位）'}`;
  if (!list || list.free <= 0) return '';
  const room = Math.max(0, list.free - list.margin);
  return bytes - Math.min(held, bytes) > room ? '设备 /data 剩余空间不足（设备始终保留 64 MB 给数据库和日志）' : '';
}

/* ---- models.status ---- */

export type VerifyState = 'idle' | 'running' | 'done' | 'failed';
export interface ModelsStatus {
  busy: boolean;
  verify: { path: string; state: VerifyState; done: number; total: number; sha256: string; expected: string; reason: string };
  upload: { active: boolean; path: string; phase: 'receiving' | 'hashing'; received: number; total: number; hashed: number };
}

const VERIFY_STATES: readonly string[] = ['idle', 'running', 'done', 'failed'];

export function parseModelsStatus(raw: unknown): ModelsStatus {
  const d = isRecord(raw) ? raw : {};
  const v = isRecord(d.verify) ? d.verify : {};
  const u = isRecord(d.upload) ? d.upload : {};
  return {
    busy: d.busy === true,
    verify: {
      path: str(v.path),
      state: VERIFY_STATES.includes(str(v.state)) ? (str(v.state) as VerifyState) : 'idle',
      done: size(v.done), total: size(v.total), sha256: str(v.sha256), expected: str(v.expected), reason: str(v.reason),
    },
    upload: {
      active: u.active === true, path: str(u.path), phase: u.phase === 'hashing' ? 'hashing' : 'receiving',
      received: size(u.received), total: size(u.total), hashed: size(u.hashed),
    },
  };
}

const VERIFY_REASONS: Record<string, string> = {
  missing: '设备上没有这个文件',
  mismatch: '文件内容与上传时记录的 SHA-256 不一致，文件可能已损坏，建议删除后重新上传',
  io: '读取文件失败',
  memory: '设备内存不足，无法开始校验',
};

export function verifyFailureText(reason: string): string {
  return VERIFY_REASONS[reason] ?? '校验失败';
}

/* ---- HTTP endpoint ---- */

/** Same rule as the firmware upload: plain HTTP beside the socket, so only
 *  against the origin that served the page. `tokenSlot` mirrors the session
 *  store's localStorage key for the socket. */
export function modelsEndpoint(deviceKey: string | null, loc: { protocol: string; host: string }): { url: string; tokenSlot: string } | null {
  if (!deviceKey || (loc.protocol !== 'http:' && loc.protocol !== 'https:')) return null;
  if (deviceKey !== 'self' && deviceKey !== `lan:${loc.host}`) return null;
  const ws = `${loc.protocol === 'https:' ? 'wss' : 'ws'}://${loc.host}/nyalink`;
  return { url: `${loc.protocol}//${loc.host}/models/upload`, tokenSlot: `nyalink.token:${ws}` };
}

export function modelsUploadUrl(url: string, path: string): string {
  return `${url}?path=${encodeURIComponent(path)}`;
}

/* ---- progress ---- */

/** "3 分 20 秒" / "45 秒" / "1 小时 5 分"; '' when it cannot be said. */
export function fmtDuration(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return '';
  const s = Math.round(seconds);
  if (s < 60) return `${s} 秒`;
  if (s < 3600) return `${Math.floor(s / 60)} 分 ${s % 60} 秒`;
  return `${Math.floor(s / 3600)} 小时 ${Math.floor((s % 3600) / 60)} 分`;
}

export interface RateSample {
  at: number;
  bytes: number;
}

/** Bytes per second over the samples held (a sliding window the caller
 *  trims); 0 until there is something to divide. */
export function transferRate(samples: readonly RateSample[]): number {
  if (samples.length < 2) return 0;
  const first = samples[0]!;
  const last = samples[samples.length - 1]!;
  const ms = last.at - first.at;
  return ms > 0 && last.bytes > first.bytes ? ((last.bytes - first.bytes) * 1000) / ms : 0;
}

/** Add a sample and drop the ones older than `windowMs`. A counter that went
 *  backwards (a piece that is sent again) starts the window over. */
export function pushSample(samples: RateSample[], at: number, bytes: number, windowMs = 10000): RateSample[] {
  const last = samples[samples.length - 1];
  const kept = last && bytes < last.bytes ? [] : samples.filter((s) => at - s.at <= windowMs);
  kept.push({ at, bytes });
  return kept;
}
