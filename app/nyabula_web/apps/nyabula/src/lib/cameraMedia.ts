export const CAMERA_MAX_SEGMENT = 512 * 1024;
export const CAMERA_CHUNK_BYTES = 12288;
export const CAMERA_MAX_PHOTO = 1024 * 1024;

export interface CameraStatus {
  available: boolean; running: boolean; phase: 'idle' | 'starting' | 'streaming' | 'stopping' | 'error';
  width: number; height: number; frames: number; lastFrameMs: number;
  readers: number; bufferBytes: number; lastError: number;
}
export interface CalibrationPoint { u: number; v: number; x: number; y: number }
export interface CameraCalibration {
  revision: number; calibrated: boolean; width?: number; height?: number; mode?: number;
  points?: CalibrationPoint[]; rmsError?: number; maxError?: number; acceptable?: boolean;
}
export interface CameraSegment {
  kind: 'init' | 'fragment'; generation: number; sequence: number; bytes: Uint8Array;
}

const object = (value: unknown): value is Record<string, unknown> => !!value && typeof value === 'object' && !Array.isArray(value);
const integer = (value: unknown, max = Number.MAX_SAFE_INTEGER): value is number => typeof value === 'number' && Number.isSafeInteger(value) && value >= 0 && value <= max;
const finite = (value: unknown): value is number => typeof value === 'number' && Number.isFinite(value);

export function isCameraStatus(value: unknown): value is CameraStatus {
  if (!object(value)) return false;
  return typeof value.available === 'boolean' && typeof value.running === 'boolean' &&
    ['idle', 'starting', 'streaming', 'stopping', 'error'].includes(String(value.phase)) &&
    integer(value.width, 640) && integer(value.height, 480) &&
    integer(value.frames) && integer(value.readers) && integer(value.bufferBytes, 2 * 1024 * 1024) &&
    integer(value.lastFrameMs) && finite(value.lastError);
}

export function isCameraCalibration(value: unknown): value is CameraCalibration {
  if (!object(value) || !integer(value.revision) || typeof value.calibrated !== 'boolean') return false;
  if (!value.calibrated) return true;
  return value.width === 640 && value.height === 480 && value.mode === 6 &&
    Array.isArray(value.points) && value.points.length === 5 && value.points.every(p => object(p) &&
      finite(p.u) && p.u >= 0 && p.u <= 1 && finite(p.v) && p.v >= 0 && p.v <= 1 &&
      finite(p.x) && Math.abs(p.x) <= 1 && finite(p.y) && Math.abs(p.y) <= 1) &&
    finite(value.rmsError) && value.rmsError >= 0 && finite(value.maxError) && value.maxError >= 0 &&
    typeof value.acceptable === 'boolean';
}

export function decodeCameraChunk(value: unknown, max: number): { offset: number; total: number; bytes: Uint8Array } {
  if (!object(value) || !integer(value.total, max) || value.total === 0 ||
      !integer(value.offset, value.total - 1) || typeof value.data !== 'string' ||
      value.data.length === 0 || value.data.length > CAMERA_CHUNK_BYTES / 3 * 4 ||
      value.data.length % 4 !== 0 || !/^[A-Za-z0-9+/]*={0,2}$/.test(value.data)) {
    throw Error('设备返回了无效的画面数据');
  }
  const raw = atob(value.data);
  if (!raw.length || raw.length > CAMERA_CHUNK_BYTES || value.offset + raw.length > value.total) {
    throw Error('画面分片长度不一致');
  }
  return { offset: value.offset, total: value.total, bytes: Uint8Array.from(raw, c => c.charCodeAt(0)) };
}

/** A device can replace an incomplete frame with a new INIT after a lag.
 * Every other discontinuity is rejected; never splice two frames together. */
export class CameraAssembler {
  private generation = -1;
  private sequence = -1;
  private buffer: Uint8Array | null = null;
  private used = 0;
  reset() { this.generation = this.sequence = -1; this.buffer = null; this.used = 0; }
  accept(value: unknown): CameraSegment | null {
    if (!object(value) || !integer(value.generation, 0xffffffff) || !integer(value.sequence) ||
        (value.kind !== 'init' && value.kind !== 'fragment')) throw Error('画面序号无效');
    const part = decodeCameraChunk(value, value.kind === 'init' ? 4096 : CAMERA_MAX_SEGMENT);
    if (value.kind === 'init') {
      if (part.offset !== 0 || part.total !== part.bytes.length || value.sequence !== 0) throw Error('画面初始化数据不完整');
      this.reset(); this.generation = value.generation;
      return { kind: 'init', generation: value.generation, sequence: 0, bytes: part.bytes };
    }
    if (this.generation !== value.generation || value.sequence === 0) throw Error('画面已更新，请重新开启');
    if (!this.buffer) {
      if (part.offset !== 0 || (this.sequence >= 0 && value.sequence !== this.sequence + 1)) throw Error('画面分片中断');
      this.buffer = new Uint8Array(part.total); this.used = 0; this.sequence = value.sequence;
    }
    if (this.sequence !== value.sequence || part.total !== this.buffer.length || part.offset !== this.used) throw Error('画面分片顺序错误');
    this.buffer.set(part.bytes, this.used); this.used += part.bytes.length;
    if (this.used !== this.buffer.length) return null;
    const bytes = this.buffer; this.buffer = null; this.used = 0;
    return { kind: 'fragment', generation: value.generation, sequence: value.sequence, bytes };
  }
}

export function cameraCodec(init: Uint8Array): string {
  if (init.length < 8 || String.fromCharCode(...init.slice(4, 8)) !== 'ftyp') throw Error('设备未返回 MP4 初始化数据');
  for (let i = 4; i + 8 < init.length; i++) {
    if (init[i] === 0x61 && init[i + 1] === 0x76 && init[i + 2] === 0x63 && init[i + 3] === 0x43 && init[i + 4] === 1) {
      return 'avc1.' + [init[i + 5], init[i + 6], init[i + 7]].map(b => b.toString(16).padStart(2, '0')).join('');
    }
  }
  throw Error('画面缺少 H.264 编码配置');
}
