import { describe, it, expect } from 'vitest';
import { EyeEngine } from '../src/engine.js';

function setup() {
  const handlers = new Map<string, (e: unknown) => void>();
  const events: unknown[] = [];
  const canvas = {
    getContext: () => null,
    getBoundingClientRect: () => ({ left: 100, top: 50, width: 800, height: 480 }),
    addEventListener: (n: string, cb: (e: unknown) => void) => handlers.set(n, cb),
    removeEventListener: (n: string) => handlers.delete(n),
    setPointerCapture: () => {},
    hasPointerCapture: () => false,
  };
  const engine = new EyeEngine(canvas as unknown as HTMLCanvasElement, {
    onInteraction: (...args) => events.push(args),
  });
  const send = (name: string, patch = {}) =>
    handlers.get(name)?.({ pointerId: 1, pointerType: 'mouse', isPrimary: true, button: 0, clientX: 900, clientY: 50, ...patch });
  return { engine, events, handlers, send };
}

describe('drag to set gaze', () => {
  it('ignores a hovering pointer and normalizes canvas-local coordinates on drag', () => {
    const { engine, send, events } = setup();
    send('pointermove');
    expect(events).toHaveLength(0);
    send('pointerdown');
    expect(events).toEqual([[1, -0.92, 'down']]);
    send('pointermove', { clientX: 500, clientY: 50 + 480 * 0.46 });
    expect(events[1]).toEqual([0, 0, 'move']);
    engine.destroy();
  });

  it('looks exactly where the device will: the target is not damped', () => {
    const { engine, send } = setup();
    send('pointerdown');
    engine.step(1 / 60);
    // eyes.gaze carries (1, -0.92) and nyabula_eye_engine_set_gaze() uses it as is.
    expect(engine.tgt.gazeX).toBeCloseTo(1, 5);
    expect(engine.tgt.gazeY).toBeCloseTo(-0.92, 5);
    engine.destroy();
  });

  it('captures one touch, ignores a second finger, and releases on cancellation', () => {
    const { engine, send, events, handlers } = setup();
    send('pointermove', { pointerType: 'touch' });
    expect(events).toHaveLength(0);
    send('pointerdown', { pointerType: 'touch' });
    send('pointerdown', { pointerId: 2, pointerType: 'touch', isPrimary: false });
    send('pointermove', { pointerId: 2, pointerType: 'touch' });
    expect(events).toHaveLength(1);
    send('pointercancel', { pointerType: 'touch' });
    expect(events).toHaveLength(2);
    send('lostpointercapture');
    expect(events).toHaveLength(2);
    engine.destroy();
    expect(handlers.size).toBe(0);
  });
});
