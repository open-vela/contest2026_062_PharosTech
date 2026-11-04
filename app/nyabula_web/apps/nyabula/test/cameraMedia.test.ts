import { describe, expect, it } from 'vitest';
import { CameraAssembler, CAMERA_MAX_SEGMENT, cameraCodec, decodeCameraChunk, isCameraCalibration, isCameraStatus } from '../src/lib/cameraMedia';

const chunk = (kind: 'init' | 'fragment', sequence: number, offset: number, total: number, bytes: number[], generation = 1) =>
  ({ kind, sequence, offset, total, generation, data: btoa(String.fromCharCode(...bytes)), key: sequence === 10 });

describe('camera packet boundaries', () => {
  it('joins ordered fragments after an init and accepts a fresh init after a lag', () => {
    const stream = new CameraAssembler();
    expect(stream.accept(chunk('init', 0, 0, 2, [1, 2]))?.kind).toBe('init');
    expect(stream.accept(chunk('fragment', 10, 0, 4, [3, 4]))).toBeNull();
    expect([...stream.accept(chunk('fragment', 10, 2, 4, [5, 6]))!.bytes]).toEqual([3, 4, 5, 6]);
    expect(stream.accept(chunk('fragment', 11, 0, 3, [7]))).toBeNull();
    stream.accept(chunk('init', 0, 0, 2, [1, 2]));
    expect(stream.accept(chunk('fragment', 20, 0, 2, [8, 9]))?.sequence).toBe(20);
  });
  it('rejects a frame before init, missing chunks, mixed frames, and generation changes', () => {
    const stream = new CameraAssembler();
    expect(() => stream.accept(chunk('fragment', 10, 0, 2, [1, 2]))).toThrow();
    stream.accept(chunk('init', 0, 0, 2, [1, 2]));
    expect(() => stream.accept(chunk('fragment', 10, 1, 2, [2]))).toThrow();
    stream.accept(chunk('fragment', 10, 0, 4, [3, 4]));
    expect(() => stream.accept(chunk('fragment', 11, 2, 4, [5, 6]))).toThrow();
    expect(() => stream.accept(chunk('fragment', 10, 2, 4, [5, 6], 2))).toThrow();
  });
  it('bounds allocations before decoding and rejects malformed lengths/base64', () => {
    expect(() => decodeCameraChunk({ total: CAMERA_MAX_SEGMENT + 1, offset: 0, data: 'AAAA' }, CAMERA_MAX_SEGMENT)).toThrow();
    for (const data of ['A', '====', 'A A=', '_w==', '', 'AAAA'.repeat(4097)]) {
      expect(() => decodeCameraChunk({ total: 5, offset: 0, data }, CAMERA_MAX_SEGMENT)).toThrow();
    }
    expect(() => decodeCameraChunk({ total: 1, offset: 0, data: 'AAAA' }, CAMERA_MAX_SEGMENT)).toThrow();
    expect(() => decodeCameraChunk({ total: 4, offset: -.1, data: 'AAAA' }, CAMERA_MAX_SEGMENT)).toThrow();
  });
  it('takes the codec from the actual init segment', () => {
    const init = Uint8Array.from([0, 0, 0, 8, ...[...'ftyp'].map(c => c.charCodeAt(0)), 0, 0, 0, 12, ...[...'avcC'].map(c => c.charCodeAt(0)), 1, 100, 0, 31, 0]);
    expect(cameraCodec(init)).toBe('avc1.64001f');
    expect(() => cameraCodec(new Uint8Array(20))).toThrow();
  });
  it('does not treat incomplete status or calibration as available', () => {
    expect(isCameraStatus({ available: true })).toBe(false);
    expect(isCameraCalibration({ calibrated: true, revision: 1 })).toBe(false);
    expect(isCameraCalibration({ calibrated: false, revision: 0 })).toBe(true);
    expect(isCameraCalibration({ calibrated: false, revision: NaN })).toBe(false);
  });
});
