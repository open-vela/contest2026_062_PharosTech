import { describe, it, expect } from 'vitest';
import { EyeEngine } from '../src/engine.js';
import type { EyeState } from '../src/types.js';

/** Minimal canvas stub: engine must be constructible without a 2D context. */
function stubCanvas(): HTMLCanvasElement {
  return {
    getContext: () => null,
    addEventListener: () => {},
    removeEventListener: () => {},
    getBoundingClientRect: () => ({ left: 0, top: 0, width: 800, height: 480 }),
  } as unknown as HTMLCanvasElement;
}

describe('EyeEngine', () => {
  it('interpolates cur toward tgt and converges', () => {
    const e = new EyeEngine(stubCanvas());
    e.setMode('surprise'); // tgt.pupilW = 0.98 regardless of light
    for (let i = 0; i < 300; i++) e.step(1 / 60);
    expect(Math.abs(e.cur.pupilW - 0.98)).toBeLessThan(0.01);
    expect(Math.abs(e.cur.irisScale - 1.14)).toBeLessThan(0.01);
  });

  it('sets correct targets on mode switch', () => {
    const e = new EyeEngine(stubCanvas());
    e.setMode('angry');
    e.step(1 / 60);
    expect(e.tgt.lidSlant).toBe(1);
    expect(e.tgt.pupilW).toBeCloseTo(0.13, 5);
    expect(e.tgt.lidTop).toBeCloseTo(0.32, 5);

    e.setMode('happy');
    e.step(1 / 60);
    expect(e.tgt.botCurve).toBe(1);
    expect(e.tgt.lidBot).toBeCloseTo(0.28, 5);
  });

  it('accepts a protocol EyeState sample without throwing', () => {
    const e = new EyeEngine(stubCanvas());
    const now = Date.now();
    const sample: EyeState = {
      seq: 421,
      t: now,
      expression: { mode: 'sleepy', since: now - 1234, lightLevel: 0.55 },
      gaze: { mode: 'target', x: 0.3, y: -0.2, holdUntil: now + 2000 },
      appearance: { irisLeft: '#38e06e', irisRight: '#ffb830' },
      scene: { type: 'pairing', style: 'full', since: now, options: {}, payload: { code: '483921' } },
      oneShot: [{ type: 'blink', at: now + 50 }],
    };
    expect(() => e.applyRemoteState(sample, 0)).not.toThrow();
    expect(e.mode).toBe('sleepy');
    expect(e.lightLvl).toBeCloseTo(0.55, 5);
    expect(e.irisHexR).toBe('#ffb830');
    // Scene goes through the closing state machine before becoming active.
    for (let i = 0; i < 120; i++) e.step(1 / 60);
    expect(e.sceneType).toBe('pairing');
    // Unknown fields must be ignored (protocol tolerance).
    expect(() =>
      e.applyRemoteState({ expression: { mode: 'idle', since: now, lightLevel: 0.5 }, extra: 1 } as EyeState),
    ).not.toThrow();
  });

  it('shows all 26 scenes and steps without throwing', () => {
    const scenes = [
      'music', 'timer', 'weather', 'battery', 'pairing',
      'alarm', 'call', 'task', 'stopwatch', 'calendar', 'sleep-timer',
      'network', 'audio', 'eq', 'caption', 'briefing', 'privacy',
      'identity', 'memory', 'devices', 'system', 'health', 'presence',
      'companion', 'home', 'subwoofer',
    ];
    const e = new EyeEngine(stubCanvas());
    for (const type of scenes) {
      expect(() => {
        e.showScene(type, 'full', {
          weather: 'storm', musicView: 'lyrics', battery: 'low',
          alarmCopy: 'task', call: 'active', task: 'done',
          network: 'bluetooth', audio: 'both', eq: 'calibrate',
        });
        for (let i = 0; i < 90; i++) e.step(1 / 60);
      }).not.toThrow();
      expect(e.sceneType).toBe(type);
    }
  });

  it('anchors scene time to remote scene.since', () => {
    const e = new EyeEngine(stubCanvas());
    const now = Date.now();
    e.applyRemoteState({
      scene: { type: 'stopwatch', style: 'full', since: now - 30_000 },
    }, 0);
    for (let i = 0; i < 120; i++) e.step(1 / 60);
    expect(e.sceneType).toBe('stopwatch');
  });

  it('sleep mode uses lid snapshot from remote state', () => {
    const e = new EyeEngine(stubCanvas());
    const now = Date.now();
    e.applyRemoteState({
      expression: { mode: 'sleep', since: now, lightLevel: 0.5, sleepLid: [0.1, 0.05] },
    });
    expect(e.mode).toBe('sleep');
    for (let i = 0; i < 400; i++) e.step(1 / 60);
    // Fully closed: lidTop + lidBot near sleepEndTop + sleepEndBot (>= .92)
    expect(e.cur.lidTop + e.cur.lidBot).toBeGreaterThan(0.9);
  });
});

describe('ambient light pupil', () => {
  const remote = (mode: string, lightLevel: number): EyeState => ({
    expression: { mode, since: Date.now(), lightLevel },
  });
  const run = (e: EyeEngine, seconds: number) => {
    for (let i = 0; i < Math.round(seconds * 60); i++) e.step(1 / 60);
  };
  /* nyabula_eye_engine_reset_target(): the baseline the device draws. */
  const baseline = (light: number) => {
    const dil = 1 - light;
    return { w: 0.10 + 0.85 * (dil * dil * 0.3 + dil * 0.7), h: 0.72 + 0.23 * dil };
  };

  it('takes the first reported level as it is and eases the following ones', () => {
    const e = new EyeEngine(stubCanvas());
    e.applyRemoteState(remote('idle', 0.2), 0);
    expect(e.lightCur).toBeCloseTo(0.2, 6);
    e.applyRemoteState(remote('idle', 0.9), 0);
    expect(e.lightLvl).toBeCloseTo(0.9, 6);
    expect(e.lightCur).toBeCloseTo(0.2, 6); // no step on arrival
    let last = e.lightCur;
    let largest = 0;
    for (let i = 0; i < 240; i++) {
      e.step(1 / 60);
      expect(e.lightCur).toBeGreaterThanOrEqual(last); // monotonic, no overshoot
      largest = Math.max(largest, e.lightCur - last);
      last = e.lightCur;
    }
    expect(largest).toBeLessThan(0.05); // a 0.7 jump never shows as one frame
    expect(e.lightCur).toBeCloseTo(0.9, 3);
  });

  it('narrows faster than it widens', () => {
    const up = new EyeEngine(stubCanvas());
    up.applyRemoteState(remote('idle', 0), 0);
    up.applyRemoteState(remote('idle', 1), 0);
    const down = new EyeEngine(stubCanvas());
    down.applyRemoteState(remote('idle', 1), 0);
    down.applyRemoteState(remote('idle', 0), 0);
    run(up, 0.25);
    run(down, 0.25);
    expect(up.lightCur).toBeCloseTo(1 - Math.exp(-4 * 0.25), 3);
    expect(1 - down.lightCur).toBeCloseTo(1 - Math.exp(-2 * 0.25), 3);
  });

  it('dark is a wide round pupil, bright a narrow slit', () => {
    const dark = new EyeEngine(stubCanvas());
    dark.applyRemoteState(remote('sleepy', 0), 0); // sleepy: no idle breathing
    dark.step(1 / 60);
    const bright = new EyeEngine(stubCanvas());
    bright.applyRemoteState(remote('sad', 1), 0);
    bright.step(1 / 60);
    expect(dark.tgt.pupilW).toBeCloseTo(baseline(0).w, 6);
    expect(dark.tgt.pupilH).toBeCloseTo(baseline(0).h, 6);
    expect(bright.tgt.pupilW).toBeCloseTo(Math.min(0.9, baseline(1).w + 0.3), 6);
    expect(bright.tgt.pupilH).toBeCloseTo(baseline(1).h, 6);
    expect(dark.tgt.pupilW / dark.tgt.pupilH).toBeGreaterThan(0.95); // round
    expect(baseline(1).w / baseline(1).h).toBeLessThan(0.15); // slit
  });

  it('stays under expressions that draw their own pupil', () => {
    const e = new EyeEngine(stubCanvas());
    e.applyRemoteState(remote('heart', 1), 0);
    for (const light of [0, 1, 0.3]) {
      e.applyRemoteState(remote('heart', light), 0);
      run(e, 2);
      expect(e.tgt.pupilW).toBe(0);
      expect(e.tgt.pupilH).toBe(0);
      expect(e.tgt.overlay).toBe(1);
    }
    e.applyRemoteState(remote('angry', 0), 0);
    run(e, 5);
    expect(e.tgt.pupilW).toBeCloseTo(0.13, 6);
    expect(e.tgt.pupilH).toBeCloseTo(0.8, 6);
    // The eased level kept following underneath and is there when it ends.
    e.applyRemoteState(remote('sleepy', 0), 0);
    e.step(1 / 60);
    expect(e.tgt.pupilW).toBeCloseTo(baseline(0).w, 3);
  });

  it('setLight() is immediate: a local slider is already a continuous motion', () => {
    const e = new EyeEngine(stubCanvas());
    e.setLight(0.1);
    expect(e.lightCur).toBeCloseTo(0.1, 6);
    e.step(1 / 60);
    expect(e.lightCur).toBeCloseTo(0.1, 6);
  });
});

describe('scene content lifetime', () => {
  const peek = (e: EyeEngine) => e as unknown as { scenePayload: Record<string, unknown>; weatherKind: string };
  const caption = (line: string): EyeState => ({
    scene: { type: 'caption', style: 'full', since: Date.now(), payload: { current_line: line }, options: { weather: 'storm' } },
  });

  it('keeps the real payload while the close transition plays, then drops it', () => {
    const e = new EyeEngine(stubCanvas());
    e.applyRemoteState(caption('真实字幕'), 0);
    for (let i = 0; i < 120; i++) e.step(1 / 60);
    expect(e.sceneType).toBe('caption');
    // What the device broadcasts on hide: scene none + a zeroed payload.
    e.applyRemoteState({ scene: { type: null, style: 'full', payload: { current_line: '' }, options: { weather: 'sunny' } } }, 0);
    e.step(1 / 60);
    expect(e.sceneType).toBe('caption'); // still on screen, lids closing
    expect(peek(e).scenePayload.current_line).toBe('真实字幕');
    expect(peek(e).weatherKind).toBe('storm');
    for (let i = 0; i < 120; i++) e.step(1 / 60);
    expect(e.sceneType).toBeNull();
    expect(peek(e).scenePayload).toEqual({});
  });

  it('holds the payload of the next scene back until the lids are shut', () => {
    const e = new EyeEngine(stubCanvas());
    e.applyRemoteState(caption('第一句'), 0);
    for (let i = 0; i < 120; i++) e.step(1 / 60);
    e.applyRemoteState({ scene: { type: 'timer', style: 'full', since: Date.now(), payload: { remaining_ms: 90_000 } } }, 0);
    e.step(1 / 60);
    expect(e.sceneType).toBe('caption');
    expect(peek(e).scenePayload).toEqual({ current_line: '第一句' });
    for (let i = 0; i < 120; i++) e.step(1 / 60);
    expect(e.sceneType).toBe('timer');
    expect(peek(e).scenePayload).toEqual({ remaining_ms: 90_000 }); // replaced, not merged
  });
});

describe('canvas background', () => {
  /* Recording 2D-context stub: logs method names + args, absorbs everything
   * else (gradients, path chains) like the pairing render test's proxy. */
  function recordingCanvas(calls: Array<{ fn: string; args: unknown[] }>): HTMLCanvasElement {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const absorb: any = new Proxy(function () {}, {
      get: (_t, k) => (k === Symbol.toPrimitive ? () => 0 : absorb),
      apply: () => absorb,
      set: () => true,
    });
    const ctx = new Proxy(
      {},
      {
        get: (_t, k) => {
          if (typeof k !== 'string') return absorb;
          return (...args: unknown[]) => {
            calls.push({ fn: k, args });
            // Gradients record their stops too; everything else is absorbed.
            return k === 'createRadialGradient'
              ? { addColorStop: (...stop: unknown[]) => calls.push({ fn: 'addColorStop', args: stop }) }
              : absorb;
          };
        },
        set: () => true,
      },
    );
    (globalThis as { Path2D?: unknown }).Path2D = function () {
      return absorb;
    };
    const noop = () => {};
    return {
      getContext: () => ctx,
      addEventListener: noop,
      removeEventListener: noop,
      getBoundingClientRect: () => ({ left: 0, top: 0, width: 800, height: 480 }),
      width: 800,
      height: 480,
    } as unknown as HTMLCanvasElement;
  }
  const tick = (e: EyeEngine) => (e as unknown as { tick(): void }).tick();

  it('clears the frame (transparent) by default instead of painting black', () => {
    const calls: Array<{ fn: string; args: unknown[] }> = [];
    const e = new EyeEngine(recordingCanvas(calls));
    tick(e);
    expect(calls.some((c) => c.fn === 'clearRect')).toBe(true);
    // No full-canvas background fill; the only fillRects are eye-local.
    expect(calls.some((c) => c.fn === 'fillRect' && c.args[0] === 0 && c.args[1] === 0
      && c.args[2] === 800 && c.args[3] === 480)).toBe(false);
  });

  it('draws the iris with the device geometry and recolouring', () => {
    const calls: Array<{ fn: string; args: unknown[] }> = [];
    const e = new EyeEngine(recordingCanvas(calls));
    tick(e);
    // 800x480 -> panel radius 128 px; device R = 178/180 of it, globe = R - 2 device px.
    const R = (128 * 178) / 180;
    const G = (R * 176) / 178;
    const gradients = calls.filter((c) => c.fn === 'createRadialGradient').map((c) => c.args as number[]);
    expect(gradients.some((g) => g.slice(0, 5).every((v) => v === 0) && Math.abs(g[5] - G) < 1e-9)).toBe(true);
    expect(gradients.some((g) => g.slice(0, 5).every((v) => v === 0) && Math.abs(g[5] - R) < 1e-9)).toBe(true);
    // Default iris #38e06e: green saturates (0xa0 * 224 / 0x80 > 255) until the knee.
    const stops = calls.filter((c) => c.fn === 'addColorStop').map((c) => c.args as [number, string]);
    expect(stops).toContainEqual([0, 'rgba(70,255,137,1)']);
    const knee = stops.find(([, color]) => color === 'rgba(63,255,125,1)');
    expect(knee?.[0]).toBeCloseTo((140 / 255) * ((160 - (255 * 128) / 224) / 32), 9);
    expect(stops).toContainEqual([1, 'rgba(16,66,32,1)']);
  });

  it('paints a solid backdrop when background is a color', () => {
    const calls: Array<{ fn: string; args: unknown[] }> = [];
    const e = new EyeEngine(recordingCanvas(calls), { background: '#000' });
    tick(e);
    expect(calls.some((c) => c.fn === 'clearRect')).toBe(false);
    expect(calls.some((c) => c.fn === 'fillRect' && c.args[0] === 0 && c.args[1] === 0
      && c.args[2] === 800 && c.args[3] === 480)).toBe(true);
  });
});

describe('pairing QR', () => {
  it('encodes host+port+code as a nyabula:// pair URI', async () => {
    const { makePairingQr, pairingQrText } = await import('../src/pairingQr.js');
    const payload = { code: '483921', host: '192.168.1.23', port: 7788 };
    expect(pairingQrText(payload)).toBe('nyabula://pair?host=192.168.1.23:7788&code=483921');
    const qr = makePairingQr(payload);
    expect(qr).not.toBeNull();
    // Valid QR versions are 21..177 modules per side, always odd.
    expect(qr!.size).toBeGreaterThanOrEqual(21);
    expect(qr!.size % 2).toBe(1);
    // Finder pattern corners are dark in every QR code.
    expect(qr!.isDark(0, 0)).toBe(true);
    expect(qr!.isDark(0, qr!.size - 1)).toBe(true);
    expect(qr!.isDark(qr!.size - 1, 0)).toBe(true);
  });

  it('encodes code only when host is missing, null without code', async () => {
    const { makePairingQr, pairingQrText } = await import('../src/pairingQr.js');
    expect(pairingQrText({ code: '000042' })).toBe('nyabula://pair?code=000042');
    expect(pairingQrText({})).toBeNull();
    expect(makePairingQr({})).toBeNull();
  });

  it('renders the pairing scene without throwing when a 2D context exists', () => {
    // Universal callable proxy: any property access / call returns itself, so
    // arbitrary Canvas2D API chains (gradients, paths, ...) are absorbed.
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const anyCtx: any = new Proxy(function () {}, {
      get: (_t, k) => {
        if (k === Symbol.toPrimitive) return () => 0;
        return k === 'width' ? 10 : anyCtx;
      },
      apply: () => anyCtx,
      set: () => true,
    });
    // Node has no Path2D; construction returns the absorbing proxy so any
    // path method chain (moveTo, arc, ...) is a no-op.
    (globalThis as { Path2D?: unknown }).Path2D = function () {
      return anyCtx;
    };
    const noop = () => {};
    const canvas = {
      getContext: () => anyCtx,
      addEventListener: noop,
      removeEventListener: noop,
      getBoundingClientRect: () => ({ left: 0, top: 0, width: 800, height: 480 }),
      width: 800,
      height: 480,
    } as unknown as HTMLCanvasElement;
    const e = new EyeEngine(canvas);
    const now = Date.now();
    e.applyRemoteState({
      scene: {
        type: 'pairing', style: 'full', since: now,
        payload: { code: '483921', host: '10.0.0.2', port: 7788 },
      },
    }, 0);
    for (let i = 0; i < 120; i++) e.step(1 / 60);
    expect(e.sceneType).toBe('pairing');
    // tick() = step + render; drives drawPairingScene (QR + big digits).
    expect(() => (e as unknown as { tick(): void }).tick()).not.toThrow();
  });
});
