import { afterEach, expect, it, vi } from 'vitest';
import { decodeIntercom, encodeIntercom, isIntercomState, IntercomPlayback } from '../src/lib/intercomAudio';
afterEach(() => vi.unstubAllGlobals());

it('uses bounded signed little-endian PCM and rejects mismatched packets', () => {
  const data = encodeIntercom(Uint8Array.from([0, 128, 255, 127, 232, 3]));
  const value = decodeIntercom({ sequence: 1, samples: 3, data });
  expect([...value.samples]).toEqual([-1, 32767 / 32768, 1000 / 32768]);
  expect(() => decodeIntercom({ sequence: 0, samples: 4, data })).toThrow();
  expect(() => decodeIntercom({ sequence: -1, samples: 3, data })).toThrow();
  expect(() => encodeIntercom(new Uint8Array(3202))).toThrow();
  expect(() => encodeIntercom(new Uint8Array(3))).toThrow();
  expect(isIntercomState({ phase: 'listening' })).toBe(false);
});
it('bounds playback latency, ignores duplicates and closes its audio context', async () => {
  const nodes: any[] = [];
  const context = { currentTime: 0, destination: {}, resume: vi.fn(async () => {}), close: vi.fn(async () => {}),
    createBuffer: vi.fn((_channels: number, size: number) => ({ getChannelData: () => new Float32Array(size) })), createBufferSource: () => {
      const node = { buffer: null, connect: vi.fn(), disconnect: vi.fn(), start: vi.fn(), stop: vi.fn(), onended: null }; nodes.push(node); return node;
    } };
  vi.stubGlobal('AudioContext', class { constructor() { return context; } });
  const player = await IntercomPlayback.open();
  const data = encodeIntercom(new Uint8Array(3200));
  for (let sequence = 0; sequence < 20; sequence++) player.feed({ sequence, samples: 1600, data });
  expect(nodes.some(node => node.stop.mock.calls.length > 0)).toBe(true);
  expect(nodes.filter(node => !node.stop.mock.calls.length).length).toBeLessThanOrEqual(5);
  const count = nodes.length; player.feed({ sequence: 19, samples: 1600, data }); expect(nodes).toHaveLength(count);
  await player.close(); await player.close(); expect(context.close).toHaveBeenCalledTimes(1);
});
it('the actual worklet emits 100 ms packets and flushes the final partial packet', async () => {
  let Capture: any;
  vi.stubGlobal('AudioWorkletProcessor', class { port = { onmessage: null, postMessage: vi.fn() }; });
  vi.stubGlobal('registerProcessor', (_: string, ctor: any) => { Capture = ctor; });
  await import('../src/lib/intercomWorklet.js');
  const capture = new Capture();
  const samples = new Float32Array(1600); samples[0] = -1; samples[1] = 1;
  capture.process([[samples]]);
  const packet = capture.port.postMessage.mock.calls[0][0];
  expect(packet.type).toBe('pcm'); expect(packet.buffer.byteLength).toBe(3200);
  expect([...new Uint8Array(packet.buffer).slice(0, 4)]).toEqual([0, 128, 255, 127]);
  capture.process([[new Float32Array(100)]]); capture.port.onmessage({ data: 'flush' });
  expect(capture.port.postMessage.mock.calls[1][0].buffer.byteLength).toBe(200);
  expect(capture.port.postMessage.mock.calls[2][0]).toEqual({ type: 'flushed' });
});
