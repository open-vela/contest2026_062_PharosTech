/*
 * EyeEngine — Nyabula cat-eye vector animation engine.
 * Started as a port of 工具/cat_eyes_demo.html; all module-level mutable state
 * from the demo lives on the instance.
 *
 * The expression eye (iris base, fibers, pupil, overlays, lids) mirrors the
 * DEVICE renderer app/nyabula/src/nyabula_eye_renderer_lvgl.c, which is the
 * source of truth because it is what the owner sees on the hardware. Where
 * the demo and the device disagree the device wins; each such place names the
 * C function it follows. Do not "optimize" visuals without checking the C.
 */
import {
  EYE_PARAMS,
  EyeParams,
  mkParams,
  SPEED,
  TIMING,
  IRIS_DEFAULT,
  SceneStyle,
} from './params.js';
import { MATERIAL_ICONS } from './materialIcons.js';
import { makePairingQr, pairingQrText, type PairingQr } from './pairingQr.js';
import type { EyeState } from './types.js';

const lerp = (a: number, b: number, t: number) => a + (b - a) * t;
const clamp = (v: number, a: number, b: number) => (v < a ? a : v > b ? b : v);
const GEOMETRY = EYE_PARAMS.geometry;
/** Eye radius R as a fraction of the round panel radius (device: 178 / 180). */
const EYE_RADIUS_RATIO = GEOMETRY.eyeRadiusPx / (GEOMETRY.panelSizePx / 2);
/** One device pixel expressed in units of R. */
const DEVICE_PX = 1 / GEOMETRY.eyeRadiusPx;
/** The fixed iris globe, eye_globe_radius() = R - 2px, in units of R. */
const GLOBE_RATIO = 1 - GEOMETRY.globeInsetPx * DEVICE_PX;
/** The device bakes the iris disc in grey 0x80 and recolours it per frame. */
const BAKE_GREY = 0x80;

const FONT_TITLE_CN = '"NyabulaTitle","MiSans","Microsoft YaHei",sans-serif';
const FONT_BODY_CN = '"NyabulaBody","MiSans","Microsoft YaHei",sans-serif';
const FONT_EN = '"NyabulaEnglish","Times New Roman",serif';
const FONT_MIXED = '"NyabulaEnglish","Times New Roman","NyabulaBody","MiSans",sans-serif';
const fontTitle = (px: number) => `${px | 0}px ${FONT_TITLE_CN}`;
const fontBody = (px: number) => `600 ${px | 0}px ${FONT_BODY_CN}`;
const fontEnglish = (px: number) => `700 ${px | 0}px ${FONT_EN}`;
const fontMixed = (px: number) => `700 ${px | 0}px ${FONT_MIXED}`;

function cubicBezier01(t: number, c1: number, c2: number): number {
  t = clamp(t, 0, 1);
  const u = 1 - t;
  return 3 * u * u * t * c1 + 3 * u * t * t * c2 + t * t * t;
}
type RGB = [number, number, number];
function hexRgb(h: string): RGB {
  h = h.replace('#', '');
  return [
    parseInt(h.substring(0, 2), 16),
    parseInt(h.substring(2, 4), 16),
    parseInt(h.substring(4, 6), 16),
  ];
}
function rgba(c: RGB | number[], a: number): string {
  return `rgba(${c[0] | 0},${c[1] | 0},${c[2] | 0},${a})`;
}
function shade(c: RGB | number[], f: number): number[] {
  return c.map((v) => clamp(v * f, 0, 255));
}

/* Iris disc gradient, bit-for-bit the device's: bake_base() rasterises the
 * four stops in grey (shade(0x80, k), truncated like channel()), and
 * bake_base_composite() recolours each pixel as min(255, grey * c / 0x80).
 * The clamp therefore happens AFTER interpolation, which keeps a saturated
 * channel at 255 for longer than interpolating between pre-clamped colours
 * does; the extra "knee" stops reproduce that. */
const IRIS_BAKE_STOPS: ReadonlyArray<readonly [number, number]> = (
  [[0, 1.25], [140 / 255, 1], [217 / 255, 0.55], [1, 0.30]] as const
).map(([at, k]) => [at, Math.floor(clamp(BAKE_GREY * k, 0, 255))] as const);

function bakedGrey(t: number): number {
  for (let i = 1; i < IRIS_BAKE_STOPS.length; i++) {
    const [t0, g0] = IRIS_BAKE_STOPS[i - 1];
    const [t1, g1] = IRIS_BAKE_STOPS[i];
    if (t <= t1) return lerp(g0, g1, (t - t0) / (t1 - t0));
  }
  return IRIS_BAKE_STOPS[IRIS_BAKE_STOPS.length - 1][1];
}

const irisStopCache = new Map<string, Array<[number, string]>>();
function irisStops(c: RGB): Array<[number, string]> {
  const key = c.join(',');
  let stops = irisStopCache.get(key);
  if (!stops) {
    const at = new Set(IRIS_BAKE_STOPS.map(([t]) => t));
    const [t0, g0] = IRIS_BAKE_STOPS[0];
    const [t1, g1] = IRIS_BAKE_STOPS[1];
    for (const v of c) {
      // Where this channel stops being clamped: grey * v / 0x80 == 255.
      const knee = (255 * BAKE_GREY) / Math.max(v, 1);
      if (knee < g0 && knee > g1) at.add(lerp(t0, t1, (g0 - knee) / (g0 - g1)));
    }
    stops = [...at].sort((a, b) => a - b).map((t) => {
      const grey = bakedGrey(t);
      return [t, rgba(c.map((v) => Math.min(255, (grey * v) / BAKE_GREY)), 1)] as [number, string];
    });
    if (irisStopCache.size > 16) irisStopCache.clear();
    irisStopCache.set(key, stops);
  }
  return stops;
}

/* Heart outline, as nyabula_eye_renderer_create() samples it: 96 points of
 * the curve (x^2 + y^2 - 1)^3 = x^2 y^3, found by bisection along each ray,
 * scaled by 1 / 1.15 and flipped to screen coordinates. */
const HEART_POINTS: ReadonlyArray<readonly [number, number]> = Array.from({ length: 96 }, (_, i) => {
  const angle = (i * Math.PI * 2) / 96;
  const dx = Math.cos(angle);
  const dy = Math.sin(angle);
  let low = 0;
  let high = 3;
  for (let n = 0; n < 12; n++) {
    const mid = (low + high) / 2;
    const x = dx * mid;
    const y = dy * mid;
    const q = x * x + y * y - 1;
    if (q * q * q - x * x * y * y * y <= 0) low = mid;
    else high = mid;
  }
  return [(dx * low) / 1.15, (-dy * low) / 1.15] as const;
});

/** clamp_to_eye_globe(): pull a point back inside a circle of radius `limit`. */
function clampToGlobe(x: number, y: number, limit: number): [number, number] {
  const max = Math.max(0, limit);
  const len = Math.hypot(x, y);
  if (len <= max || len === 0) return [x, y];
  return [(x * max) / len, (y * max) / len];
}

const SCENE_CLOSE_TIME = TIMING.sceneCloseTime;
const SCENE_OPEN_TIME = TIMING.sceneOpenTime;
const SCENE_FADE_TIME = TIMING.sceneFadeTime;
const [SCENE_EASE_C1, SCENE_EASE_C2] = TIMING.sceneEase;
function sceneEase(t: number): number {
  return cubicBezier01(clamp(t, 0, 1), SCENE_EASE_C1, SCENE_EASE_C2);
}

const WEATHER_META: Record<string, string[]> = {
  sunny: ['28°', '晴朗', '紫外', '4', '体感', '29°'],
  cloudy: ['21°', '多云', '湿度', '66%', '体感', '20°'],
  rain: ['23°', '小雨', '湿度', '78%', '体感', '22°'],
  storm: ['19°', '雷雨', '风速', '24', '湿度', '86%'],
  snow: ['1°', '降雪', '湿度', '91%', '体感', '-2°'],
  fog: ['16°', '有雾', '能见', '0.8', '湿度', '95%'],
};

interface ZzzParticle {
  side: number;
  ox: number;
  y: number;
  vy: number;
  sway: number;
  ph: number;
  size: number;
  life: number;
  rot: number;
}

export interface EyeEngineOptions {
  /** Fired on canvas pointer interaction with normalized [-1,1] coords. */
  onInteraction?: (x: number, y: number, phase: 'down' | 'move' | 'up') => void;
  /** Attach pointer listeners to the canvas (default true). */
  attachPointer?: boolean;
  /** Also react locally to pointer (lookAt) in addition to onInteraction. */
  localLook?: boolean;
  /**
   * Canvas background. 'transparent' (default) clears the frame instead of
   * painting a full black backdrop, so the two eyes float over whatever the
   * host page renders behind the canvas. Any CSS color paints a solid fill.
   */
  background?: string | 'transparent';
}

export class EyeEngine {
  readonly canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D | null;
  private opts: EyeEngineOptions;

  private W = 0;
  private H = 0;
  private DPR = 1;

  /* Interpolated parameter state. Plain objects — never make these reactive. */
  readonly cur: EyeParams = mkParams();
  readonly tgt: EyeParams = mkParams();

  mode = 'idle';
  private modeT = 0;
  irisHexL = IRIS_DEFAULT;
  irisHexR = IRIS_DEFAULT;
  lightLvl = 0.55;
  private blinkPhase = -1;
  private blinkEyes = 3;
  private nextBlink = 2.5;
  private nextSaccade = 1.2;
  private saccade = { x: 0, y: 0 };
  private lookTarget: { x: number; y: number } | null = null;
  private lookHold = 0;
  private sleepStartTop = 0;
  private sleepStartBot = 0;
  private sleepEndTop = 0.86;
  private sleepEndBot = 0.14;
  private tPrev = 0;
  private tNow = 0;

  /* Scene state machine (see demo comments: content only swaps while both
   * eyes are closed). */
  sceneType: string | null = null;
  private scenePrevType: string | null = null;
  private scenePending: string | null = null;
  private sceneStartedAt = 0;
  private scenePrevStartedAt = 0;
  private scenePhase = 'none';
  private scenePhaseT = 0;
  private sceneLid = 0;
  sceneStyle: SceneStyle = 'full';
  private scenePendingStyle: SceneStyle | null = null;
  private sceneFade = 1;
  private scenePayload: Record<string, unknown> = {};
  /** Payload the outgoing scene of a minimal cross-fade keeps drawing with. */
  private scenePrevPayload: Record<string, unknown> | null = null;
  /* Content of a scene that is not on screen yet. It replaces the live
   * payload / options only once the lids are shut (or at the minimal
   * cross-fade swap), so the scene being left never redraws with the next
   * scene's content -- the same rule as the device's pending_scene. */
  private stagedPayload: Record<string, unknown> | null = null;
  private stagedOptions: Record<string, unknown> | null = null;
  /** Cached pairing QR matrix, rebuilt only when the encoded text changes. */
  private pairingQrCache: PairingQr | null = null;

  /* Scene option states */
  weatherKind = 'rain';
  private weatherPrevKind: string | null = null;
  private weatherFade = 1;
  musicView = 'spectrum';
  private musicPrevView: string | null = null;
  private musicViewFade = 1;
  batteryState = 'charging';
  private batteryPrevState: string | null = null;
  alarmCopy = 'name';
  callState = 'incoming';
  taskState = 'running';
  private taskPrevState: string | null = null;
  networkState = 'wifi';
  private networkPrevState: string | null = null;
  audioRoute = 'speaker';
  private audioPrevRoute: string | null = null;
  eqView = 'profile';
  private optionFade = 1;
  private optionChangedAt = -Infinity;
  /** Remote scene "since" anchor (in local tNow seconds), applied when a
   *  pending scene commits so animated progress matches the sender. */
  private sceneSinceAnchor: number | null = null;

  /* Zzz particles */
  private zzz: ZzzParticle[] = [];
  private zNext = 0;
  private zzzMask = 0;

  private rafId = 0;
  private running = false;
  private clockOffsetMs = 0;
  private lastOneShotAt = 0;
  private lastRemoteMode: string | null = null;
  private autoBlink = true;
  private blinkNonce: number | null = null;

  private materialPathCache = new Map<string, Path2D>();
  private materialLengthCache = new Map<string, number>();
  private detachFns: Array<() => void> = [];

  constructor(canvas: HTMLCanvasElement, opts: EyeEngineOptions = {}) {
    this.canvas = canvas;
    this.opts = opts;
    this.ctx = typeof canvas.getContext === 'function' ? canvas.getContext('2d') : null;
    this.tPrev = this.now();
    this.tNow = this.tPrev;
    if (opts.attachPointer !== false && typeof canvas.addEventListener === 'function') {
      this.attachPointer();
    }
  }

  /* ---------------- public API ---------------- */

  setMode(mode: string): void {
    if (this.sceneType || this.scenePending) this.hideScene();
    const previousMode = this.mode;
    if (mode === 'sleep' && previousMode !== 'sleep') {
      // Sleep closes from the current lid position, not from fully open.
      this.sleepStartTop = this.cur.lidTop;
      this.sleepStartBot = this.cur.lidBot;
      this.sleepEndBot = Math.max(0.14, this.sleepStartBot);
      this.sleepEndTop = Math.max(this.sleepStartTop, 1 - this.sleepEndBot);
      this.zNext = 0;
    }
    this.mode = mode;
    this.modeT = 0;
    if (mode === 'surprise') {
      this.blinkPhase = -1;
      this.nextBlink = 3;
    }
  }

  lookAt(x: number, y: number, hold: number = TIMING.lookHoldDefault): void {
    // nyabula_eye_engine_set_gaze(): the clamped target is used unscaled. The
    // demo damped it by 0.8 / 0.7, which made the preview look less far than
    // the device for the same eyes.gaze command.
    this.lookTarget = { x: clamp(x, -1, 1), y: clamp(y, -1, 1) };
    this.lookHold = hold;
    this.saccade.x = 0;
    this.saccade.y = 0;
  }

  releaseLook(): void {
    this.lookTarget = null;
    this.lookHold = 0;
  }

  setLight(v: number): void {
    this.lightLvl = clamp(v, 0, 1);
  }

  setIris(left: string, right: string = left): void {
    this.irisHexL = left;
    this.irisHexR = right;
  }

  blink(eyes = 3): void {
    this.blinkEyes = eyes;
    this.blinkPhase = 0;
  }

  showScene(
    type: string,
    style?: SceneStyle,
    options?: Record<string, unknown>,
    payload?: Record<string, unknown>,
  ): void {
    this.acceptSceneContent(type, options, payload);
    if (style && style !== this.sceneStyle) this.setSceneStyle(style);
    this.wakeForScene();
    if (this.sceneStyle === 'minimal' && this.sceneType && this.scenePhase === 'visible') {
      if (this.sceneType === type) return;
      this.scenePrevType = this.sceneType;
      this.scenePrevStartedAt = this.sceneStartedAt;
      this.scenePrevPayload = this.scenePayload;
      this.commitStagedSceneContent(true, false);
      this.sceneType = type;
      this.sceneStartedAt = this.sceneSinceAnchor ?? this.tNow;
      this.sceneSinceAnchor = null;
      this.sceneFade = 0;
      return;
    }
    if (this.scenePending === type && this.scenePhase.startsWith('closing')) return;
    this.scenePending = type;
    this.scenePhase = this.sceneType ? 'closing-switch' : 'closing-in';
    this.scenePhaseT = this.sceneLid * SCENE_CLOSE_TIME;
  }

  /** Leave the scene. Its payload and options stay untouched while the close
   *  transition plays; updateScene() drops them once the scene is off screen. */
  hideScene(): void {
    this.scenePending = null;
    this.scenePendingStyle = null;
    this.scenePrevType = null;
    this.scenePrevPayload = null;
    this.stagedPayload = null;
    this.stagedOptions = null;
    if (!this.sceneType && this.scenePhase === 'none') return;
    if (this.sceneStyle === 'minimal' && this.sceneType) {
      // Minimal scenes live on the closed eyelid surface; reveal the real
      // eye while the old scene stays clipped to the retreating lids.
      this.scenePhase = 'reopening-minimal-out';
      this.scenePhaseT = 0;
      this.sceneLid = 1;
      this.sceneFade = 1;
      return;
    }
    this.scenePhase = 'closing-out';
    this.scenePhaseT = this.sceneLid * SCENE_CLOSE_TIME;
  }

  setSceneStyle(style: SceneStyle): void {
    if (style === this.sceneStyle && !this.scenePendingStyle) return;
    if (!this.sceneType) {
      this.sceneStyle = style;
      this.scenePendingStyle = null;
      return;
    }
    if (this.sceneStyle === 'minimal' && style === 'full') {
      this.sceneStyle = 'full';
      this.scenePrevType = null;
      this.sceneFade = 1;
      this.scenePhase = 'reopening-in';
      this.scenePhaseT = 0;
      return;
    }
    this.scenePending = this.sceneType;
    this.scenePendingStyle = style;
    this.scenePhase = 'closing-switch';
    this.scenePhaseT = this.sceneLid * SCENE_CLOSE_TIME;
  }

  /** Ingest a protocol EyeState (event eye.state). clockOffsetMs = server
   *  epoch minus local epoch, estimated from sys.ping. */
  applyRemoteState(state: EyeState, clockOffsetMs?: number): void {
    if (typeof state.autoBlink === 'boolean') this.autoBlink = state.autoBlink;
    if (typeof state.blinkNonce === 'number') {
      if (this.blinkNonce !== null && state.blinkNonce > this.blinkNonce) this.blink(state.blinkEyes ?? 3);
      this.blinkNonce = state.blinkNonce;
    }
    if (clockOffsetMs !== undefined) this.clockOffsetMs = clockOffsetMs;
    const ex = state.expression;
    if (ex && typeof ex.mode === 'string') {
      if (typeof ex.lightLevel === 'number') this.lightLvl = clamp(ex.lightLevel, 0, 1);
      if (ex.mode !== this.mode || this.lastRemoteMode !== ex.mode) {
        const prev = this.mode;
        if (ex.mode === 'sleep' && prev !== 'sleep') {
          const lid = ex.sleepLid;
          this.sleepStartTop = lid ? lid[0] : this.cur.lidTop;
          this.sleepStartBot = lid ? lid[1] : this.cur.lidBot;
          this.sleepEndBot = Math.max(0.14, this.sleepStartBot);
          this.sleepEndTop = Math.max(this.sleepStartTop, 1 - this.sleepEndBot);
          this.zNext = 0;
        }
        this.mode = ex.mode;
        if (ex.mode === 'surprise') {
          this.blinkPhase = -1;
          this.nextBlink = 3;
        }
        this.lastRemoteMode = ex.mode;
      }
      // Align modeT to the sender's "since" anchor.
      if (typeof ex.since === 'number' && ex.since > 0) {
        this.modeT = Math.max(0, (this.localEpochNow() - ex.since) / 1000);
      }
    }
    const ap = state.appearance;
    if (ap) {
      if (ap.irisLeft) this.irisHexL = ap.irisLeft;
      if (ap.irisRight) this.irisHexR = ap.irisRight;
    }
    const gz = state.gaze;
    if (gz) {
      if (gz.mode === 'target') {
        const hold = gz.holdUntil
          ? Math.max(0, (gz.holdUntil - this.localEpochNow()) / 1000)
          : TIMING.lookHoldDefault;
        this.lookAt(gz.x, gz.y, hold);
      } else {
        this.releaseLook();
      }
    }
    const sc = state.scene;
    if (sc !== undefined && sc !== null && sc.type == null) {
      // The device reports "no scene" together with a zeroed scene_payload (and
      // the options derived from it). That content describes nothing; applying
      // it would repaint the scene that is still closing with placeholders.
      if (this.sceneType || this.scenePending) this.hideScene();
    } else if (sc !== undefined && sc !== null && sc.type != null) {
      const entering = sc.type !== this.sceneType && sc.type !== this.scenePending;
      if (!entering) this.acceptSceneContent(sc.type, sc.options, sc.payload);
      // Anchor scene-relative animation time (timers, rings) to the sender's
      // scene.since so remote peers render identical progress.
      if (typeof sc.since === 'number' && sc.since > 0) {
        const anchor = this.tNow - (this.localEpochNow() - sc.since) / 1000;
        if (sc.type === this.sceneType && !this.scenePending) {
          this.sceneStartedAt = anchor;
        } else {
          this.sceneSinceAnchor = anchor;
        }
      }
      const style = sc.style === 'minimal' ? 'minimal' : 'full';
      if (entering) {
        this.showScene(sc.type, style, sc.options, sc.payload);
      } else if (style !== this.sceneStyle) {
        this.setSceneStyle(style);
      }
    }
    if (Array.isArray(state.oneShot)) {
      for (const o of state.oneShot) {
        if (!o || typeof o.at !== 'number' || o.at <= this.lastOneShotAt) continue;
        this.lastOneShotAt = o.at;
        if (o.type === 'blink') this.blink();
      }
    }
  }

  start(): void {
    if (this.running) return;
    this.running = true;
    this.tPrev = this.now();
    const loop = () => {
      if (!this.running) return;
      this.tick();
      this.rafId = requestAnimationFrame(loop);
    };
    this.rafId = requestAnimationFrame(loop);
  }

  stop(): void {
    this.running = false;
    if (this.rafId) {
      cancelAnimationFrame(this.rafId);
      this.rafId = 0;
    }
  }

  destroy(): void {
    this.stop();
    for (const fn of this.detachFns) fn();
    this.detachFns = [];
  }

  /** Advance the simulation by dt seconds without rendering (test hook). */
  step(dt: number): void {
    this.tNow += dt;
    this.modeT += dt;
    this.applyMode();
    this.behaviors(dt);
    this.updateZzz(dt);
    this.updateScene(dt);
    for (const k in SPEED) {
      this.cur[k] = lerp(this.cur[k], this.tgt[k], 1 - Math.exp(-SPEED[k] * dt));
    }
  }

  /* ---------------- internals ---------------- */

  private now(): number {
    return (typeof performance !== 'undefined' ? performance.now() : Date.now()) / 1000;
  }

  private localEpochNow(): number {
    return Date.now() + this.clockOffsetMs;
  }

  private applySceneOptions(options: Record<string, unknown>): void {
    const setFade = () => {
      this.optionFade = 0;
      this.optionChangedAt = this.tNow;
    };
    if (typeof options.weather === 'string' && options.weather !== this.weatherKind) {
      this.weatherPrevKind = this.weatherKind;
      this.weatherKind = options.weather;
      this.weatherFade = 0;
    }
    if (typeof options.musicView === 'string' && options.musicView !== this.musicView) {
      this.musicPrevView = this.musicView;
      this.musicView = options.musicView;
      this.musicViewFade = 0;
    }
    if (typeof options.battery === 'string' && options.battery !== this.batteryState) {
      this.batteryPrevState = this.batteryState;
      this.batteryState = options.battery;
      setFade();
    }
    // alarmCopy swaps instantly in the demo (no option fade).
    if (typeof options.alarmCopy === 'string') this.alarmCopy = options.alarmCopy;
    if (typeof options.call === 'string' && options.call !== this.callState) {
      this.callState = options.call;
      setFade();
    }
    if (typeof options.task === 'string' && options.task !== this.taskState) {
      this.taskPrevState = this.taskState;
      this.taskState = options.task;
      setFade();
    }
    if (typeof options.network === 'string' && options.network !== this.networkState) {
      this.networkPrevState = this.networkState;
      this.networkState = options.network;
      setFade();
    }
    if (typeof options.audio === 'string' && options.audio !== this.audioRoute) {
      this.audioPrevRoute = this.audioRoute;
      this.audioRoute = options.audio;
      setFade();
    }
    if (typeof options.eq === 'string' && options.eq !== this.eqView) {
      this.eqView = options.eq;
      setFade();
    }
  }

  /** Route incoming scene content: live when it belongs to the scene on screen
   *  (or nothing is on screen), staged when it belongs to the scene that will
   *  replace the visible one. */
  private acceptSceneContent(
    type: string,
    options?: Record<string, unknown>,
    payload?: Record<string, unknown>,
  ): void {
    if (this.sceneType !== null && this.sceneType !== type) {
      if (options) this.stagedOptions = { ...this.stagedOptions, ...options };
      if (payload) this.stagedPayload = { ...this.stagedPayload, ...payload };
      return;
    }
    this.stagedOptions = null;
    this.stagedPayload = null;
    if (options) this.applySceneOptions(options);
    if (payload) {
      // Nothing on screen and a different scene was queued: start clean.
      const fresh = this.sceneType === null && this.scenePending !== type;
      this.scenePayload = fresh ? { ...payload } : { ...this.scenePayload, ...payload };
    }
  }

  /** Make the staged content live. `replaced` = a different scene takes over,
   *  so the old payload must not leak into it. `settle` snaps option
   *  cross-fades, used when the swap happens behind closed lids. */
  private commitStagedSceneContent(replaced: boolean, settle: boolean): void {
    if (replaced) this.scenePayload = this.stagedPayload ?? {};
    else if (this.stagedPayload) this.scenePayload = { ...this.scenePayload, ...this.stagedPayload };
    if (this.stagedOptions) {
      this.applySceneOptions(this.stagedOptions);
      if (settle) this.settleSceneOptions();
    }
    this.stagedPayload = null;
    this.stagedOptions = null;
  }

  private settleSceneOptions(): void {
    this.optionFade = 1;
    this.optionChangedAt = -Infinity;
    this.weatherFade = 1;
    this.musicViewFade = 1;
    this.weatherPrevKind = null;
    this.musicPrevView = null;
    this.batteryPrevState = null;
    this.networkPrevState = null;
    this.audioPrevRoute = null;
    this.taskPrevState = null;
  }

  /** The scene is off screen: only now may its state go. */
  private dropSceneContent(): void {
    this.sceneType = null;
    this.scenePrevType = null;
    this.scenePrevPayload = null;
    this.scenePayload = {};
  }

  private wakeForScene(): void {
    if (this.mode !== 'sleep') return;
    // Sleep is already visually closed; keep that closure instead of briefly
    // reopening the idle eye before the scene swap.
    this.sceneLid = 1;
    this.mode = 'idle';
    this.modeT = 0;
  }

  private attachPointer(): void {
    const cv = this.canvas;
    const norm = (e: PointerEvent) => {
      const rect = cv.getBoundingClientRect();
      const w = rect.width || 1;
      const h = rect.height || 1;
      const nx = clamp((e.clientX - rect.left - w / 2) / (w / 2), -1, 1);
      const ny = clamp((e.clientY - rect.top - h * 0.46) / (h / 2), -1, 1);
      return { nx, ny };
    };
    let pointerId: number | null = null;
    const onDown = (e: PointerEvent) => {
      if (!e.isPrimary || e.button !== 0 || pointerId !== null) return;
      pointerId = e.pointerId;
      cv.setPointerCapture?.(e.pointerId);
      const { nx, ny } = norm(e);
      if (this.opts.localLook !== false) this.lookAt(nx, ny);
      this.opts.onInteraction?.(nx, ny, 'down');
    };
    const onMove = (e: PointerEvent) => {
      if (pointerId !== e.pointerId) return;
      const { nx, ny } = norm(e);
      if (this.opts.localLook !== false) this.lookAt(nx, ny);
      this.opts.onInteraction?.(nx, ny, 'move');
    };
    const onUp = (e: PointerEvent) => {
      if (pointerId !== e.pointerId) return;
      pointerId = null;
      if (cv.hasPointerCapture?.(e.pointerId)) cv.releasePointerCapture(e.pointerId);
      const { nx, ny } = norm(e);
      this.opts.onInteraction?.(nx, ny, 'up');
    };
    cv.addEventListener('pointerdown', onDown);
    cv.addEventListener('pointermove', onMove);
    cv.addEventListener('pointerup', onUp);
    cv.addEventListener('pointercancel', onUp);
    cv.addEventListener('lostpointercapture', onUp);
    this.detachFns.push(() => {
      cv.removeEventListener('pointerdown', onDown);
      cv.removeEventListener('pointermove', onMove);
      cv.removeEventListener('pointerup', onUp);
      cv.removeEventListener('pointercancel', onUp);
      cv.removeEventListener('lostpointercapture', onUp);
    });
  }

  private resize(): void {
    const cv = this.canvas;
    const rect = cv.getBoundingClientRect();
    const w = Math.max(1, Math.round(rect.width));
    const h = Math.max(1, Math.round(rect.height));
    const dpr = Math.min(2, (typeof devicePixelRatio !== 'undefined' ? devicePixelRatio : 1) || 1);
    if (w !== this.W || h !== this.H || dpr !== this.DPR) {
      this.W = w;
      this.H = h;
      this.DPR = dpr;
      cv.width = w * dpr;
      cv.height = h * dpr;
    }
    this.ctx?.setTransform(this.DPR, 0, 0, this.DPR, 0, 0);
  }

  private tick(): void {
    const t = this.now();
    const dt = Math.min(0.05, t - this.tPrev);
    this.tPrev = t;
    this.tNow = t;
    this.modeT += dt;
    this.applyMode();
    this.behaviors(dt);
    this.updateZzz(dt);
    this.updateScene(dt);
    for (const k in SPEED) {
      this.cur[k] = lerp(this.cur[k], this.tgt[k], 1 - Math.exp(-SPEED[k] * dt));
    }
    // Blink easing: closing slightly faster than opening (~240ms total).
    let blink = 0;
    if (this.blinkPhase >= 0) {
      const cp = TIMING.blinkClosePortion;
      if (this.blinkPhase < cp) blink = sceneEase(this.blinkPhase / cp);
      else blink = 1 - sceneEase((this.blinkPhase - cp) / (1 - cp));
    }
    const zzzMaskTarget =
      this.mode === 'sleep' ? clamp(this.cur.lidTop + this.cur.lidBot, 0, 1) : 0;
    this.zzzMask = lerp(this.zzzMask, zzzMaskTarget, 1 - Math.exp(-SPEED.lidTop * dt));
    if (this.mode !== 'sleep' && this.zzzMask < 0.002) this.zzz.length = 0;
    this.render(blink);
  }

  /* ---------- mode target parameters (1:1 with demo applyMode) ---------- */
  applyMode(): void {
    const p = this.tgt;
    const t = this.modeT;
    // Baseline: ambient light drives the pupil (slit <-> dilated).
    const dil = 1 - this.lightLvl;
    p.pupilW = lerp(0.10, 0.95, dil * dil * 0.3 + dil * 0.7);
    p.pupilH = lerp(0.72, 0.95, dil);
    p.lidTop = 0; p.lidBot = 0; p.lidSlant = 0; p.botCurve = 0;
    p.gazeX = 0; p.gazeY = 0;
    p.irisScale = 1; p.glow = 0.5; p.overlay = 0; p.squint = 0; p.derp = 0;

    switch (this.mode) {
      case 'idle':
        p.pupilW *= 1 + Math.sin(t * 1.1) * 0.04;
        break;
      case 'curious':
        p.pupilW = Math.min(0.9, p.pupilW * 1.5 + 0.25); p.pupilH = 0.95;
        p.irisScale = 1.06; p.glow = 0.75;
        break;
      case 'happy':
        p.botCurve = 1; p.lidBot = 0.28; p.squint = 0.15;
        p.pupilW = Math.min(0.85, p.pupilW + 0.2);
        p.glow = 0.85;
        break;
      case 'processing':
        p.pupilW = 0.95; p.pupilH = 0.95; p.overlay = 1; p.glow = 0.6;
        break;
      case 'star':
      case 'heart':
        p.pupilW = 0; p.pupilH = 0; p.overlay = 1; p.irisScale = 1.08; p.glow = 1;
        p.botCurve = 0.5; p.lidBot = 0.1;
        break;
      case 'sleepy':
        p.lidTop = 0.55; p.lidBot = 0.15; p.pupilW = Math.max(p.pupilW, 0.5);
        p.gazeY = 0.25; p.glow = 0.3;
        break;
      case 'sleep': {
        // Close from the lid position captured on sleep entry.
        const sleepClose = cubicBezier01(
          clamp(t / TIMING.sleepCloseDuration, 0, 1), SCENE_EASE_C1, SCENE_EASE_C2);
        p.lidTop = lerp(this.sleepStartTop, this.sleepEndTop, sleepClose);
        p.lidBot = lerp(this.sleepStartBot, this.sleepEndBot, sleepClose);
        p.glow = lerp(0.4, 0.15, Math.min(1, t / 2));
        p.overlay = sleepClose > 0.98 ? 1 : 0;
        p.gazeY = 0.2;
        break;
      }
      case 'angry':
        p.lidTop = 0.32; p.lidSlant = 1; p.pupilW = 0.13; p.pupilH = 0.8;
        p.squint = 0.2; p.glow = 0.7;
        break;
      case 'sad':
        p.lidTop = 0.3; p.lidSlant = -0.9; p.lidBot = 0.12;
        p.pupilW = Math.min(0.9, p.pupilW + 0.3); p.gazeY = 0.3; p.glow = 0.35;
        p.overlay = t > 1.2 ? 1 : 0; // tear film appears with a delay
        break;
      case 'surprise':
        p.pupilW = 0.98; p.pupilH = 0.98; p.irisScale = 1.14; p.glow = 1;
        if (t < 0.25) p.irisScale = 1.2;
        break;
      case 'dizzy':
        p.pupilW = 0; p.pupilH = 0; p.overlay = 1;
        p.lidTop = 0.15; p.lidSlant = 0.3;
        break;
      case 'derp':
        p.derp = 1; p.pupilW = 0.8; p.pupilH = 0.86;
        p.lidTop = 0; p.glow = 0.45;
        p.gazeX = Math.sin(t * 0.7) * 0.05;
        break;
    }
  }

  /* ---------- behaviors: blink / saccade ---------- */
  private behaviors(dt: number): void {
    if (this.sceneType || this.scenePhase !== 'none') {
      this.blinkPhase = -1;
    } else if (this.blinkPhase >= 0) {
      this.blinkPhase += dt * TIMING.blinkSpeed;
      if (this.blinkPhase >= 1) this.blinkPhase = -1;
    } else if (this.mode !== 'sleep' && this.autoBlink) {
      this.nextBlink -= dt;
      if (this.nextBlink <= 0) {
        this.blinkEyes = 3;
        this.blinkPhase = 0;
        this.nextBlink = TIMING.blinkIntervalMin + Math.random() * TIMING.blinkIntervalRand;
        if (Math.random() < TIMING.doubleBlinkChance) this.nextBlink = TIMING.doubleBlinkDelay;
      }
    }
    if (this.lookTarget) {
      this.lookHold -= dt;
      if (this.lookHold <= 0) this.lookTarget = null;
    }
    if (!this.lookTarget && (this.mode === 'idle' || this.mode === 'curious' || this.mode === 'happy')) {
      this.nextSaccade -= dt;
      if (this.nextSaccade <= 0) {
        this.nextSaccade = TIMING.saccadeIntervalMin + Math.random() * TIMING.saccadeIntervalRand;
        this.saccade.x = (Math.random() * 2 - 1) * TIMING.saccadeAmpX;
        this.saccade.y = (Math.random() * 2 - 1) * TIMING.saccadeAmpY;
        if (Math.random() < TIMING.saccadeRecenterChance) {
          this.saccade.x = 0;
          this.saccade.y = 0;
        }
      }
    }
    if (this.mode === 'sleepy') {
      this.saccade.x = Math.sin(this.tNow * 0.4) * 0.15;
      this.saccade.y = 0.2;
    }
    if (this.mode === 'processing') { this.saccade.x = 0; this.saccade.y = -0.1; }
    if (this.mode === 'dizzy' || this.mode === 'derp') { this.saccade.x = 0; this.saccade.y = 0; }
    if (this.lookTarget) {
      this.tgt.gazeX = this.lookTarget.x;
      this.tgt.gazeY = this.lookTarget.y;
    } else {
      this.tgt.gazeX = clamp(this.tgt.gazeX + this.saccade.x, -1, 1);
      this.tgt.gazeY = clamp(this.tgt.gazeY + this.saccade.y, -1, 1);
    }
  }

  /* ---------- scene state machine ---------- */
  private updateScene(dt: number): void {
    if (this.optionFade < 1) {
      this.optionFade = clamp(this.optionFade + dt / 0.42, 0, 1);
      if (this.optionFade >= 1) {
        this.batteryPrevState = null;
        this.networkPrevState = null;
        this.audioPrevRoute = null;
        this.taskPrevState = null;
      }
    }
    if (this.weatherFade < 1) {
      this.weatherFade = clamp(this.weatherFade + dt / SCENE_FADE_TIME, 0, 1);
      if (this.weatherFade >= 1) this.weatherPrevKind = null;
    }
    if (this.sceneFade < 1) {
      this.sceneFade = clamp(this.sceneFade + dt / SCENE_FADE_TIME, 0, 1);
      if (this.sceneFade >= 1) {
        this.scenePrevType = null;
        this.scenePrevPayload = null;
      }
    }
    if (this.musicViewFade < 1) {
      this.musicViewFade = clamp(this.musicViewFade + dt / SCENE_FADE_TIME, 0, 1);
      if (this.musicViewFade >= 1) this.musicPrevView = null;
    }
    if (this.scenePhase === 'none' || this.scenePhase === 'visible') return;
    this.scenePhaseT += dt;
    if (this.scenePhase.startsWith('closing')) {
      const p = clamp(this.scenePhaseT / SCENE_CLOSE_TIME, 0, 1);
      this.sceneLid = sceneEase(p);
      if (p >= 1) {
        if (this.scenePhase === 'closing-out') {
          this.dropSceneContent();
          this.scenePhase = 'reopening-out';
        } else {
          this.commitStagedSceneContent(
            this.sceneType !== null && this.sceneType !== this.scenePending, true);
          this.sceneType = this.scenePending;
          this.sceneStartedAt = this.sceneSinceAnchor ?? this.tNow;
          this.sceneSinceAnchor = null;
          this.scenePending = null;
          if (this.scenePendingStyle) {
            this.sceneStyle = this.scenePendingStyle;
            this.scenePendingStyle = null;
          }
          this.scenePrevType = null;
          this.sceneFade = this.sceneStyle === 'minimal' ? 0 : 1;
          this.scenePhase = this.sceneStyle === 'minimal' ? 'visible' : 'reopening-in';
        }
        this.scenePhaseT = 0;
      }
    } else if (this.scenePhase === 'reopening-minimal-out') {
      const p = clamp(this.scenePhaseT / SCENE_OPEN_TIME, 0, 1);
      this.sceneLid = 1 - sceneEase(p);
      this.sceneFade = 1 - sceneEase(clamp(p / 0.86, 0, 1));
      if (p >= 1) {
        this.dropSceneContent();
        this.sceneLid = 0;
        this.sceneFade = 1;
        this.scenePhase = 'none';
        this.scenePhaseT = 0;
      }
    } else if (this.scenePhase.startsWith('reopening')) {
      const p = clamp(this.scenePhaseT / SCENE_OPEN_TIME, 0, 1);
      this.sceneLid = 1 - sceneEase(p);
      if (p >= 1) {
        this.sceneLid = 0;
        this.scenePhase = this.scenePhase === 'reopening-in' ? 'visible' : 'none';
        this.scenePhaseT = 0;
      }
    }
  }

  /* ---------- Zzz particles ---------- */
  /** Round panel radius in CSS px; the eye radius R is a fixed share of it. */
  private panelRadius(): number {
    return Math.min(this.W * 0.16, this.H * 0.30);
  }

  private updateZzz(dt: number): void {
    // update_z() sizes every particle quantity from R.
    const R = this.panelRadius() * EYE_RADIUS_RATIO;
    const CR = R;
    if (this.mode === 'sleep' && this.cur.lidTop + this.cur.lidBot > 0.92) {
      this.zNext -= dt;
      if (this.zNext <= 0) {
        this.zNext = 0.8 + Math.random() * 1.2;
        const vy = -(CR * 0.30 + Math.random() * CR * 0.15);
        const size = R * (0.15 + Math.random() * 0.15);
        for (const side of [-1, 1]) {
          this.zzz.push({
            side,
            ox: (Math.random() * 2 - 1) * CR * 0.55,
            y: CR * 1.1,
            vy,
            sway: CR * (0.08 + Math.random() * 0.10),
            ph: Math.random() * 7,
            size,
            life: 0,
            rot: (Math.random() - 0.5) * 0.5,
          });
        }
      }
    }
    for (let i = this.zzz.length - 1; i >= 0; i--) {
      const z = this.zzz[i];
      z.life += dt;
      z.y += z.vy * dt;
      if (z.y < -CR * 1.2) this.zzz.splice(i, 1);
    }
  }

  /* ---------- rendering ---------- */
  private render(blink: number): void {
    const ctx = this.ctx;
    if (!ctx) return;
    this.resize();
    const { W, H } = this;
    const bg = this.opts.background ?? 'transparent';
    if (bg === 'transparent') {
      ctx.clearRect(0, 0, W, H);
    } else {
      ctx.fillStyle = bg;
      ctx.fillRect(0, 0, W, H);
    }
    // CR = the physical round panel (360 px across on the device); R = the
    // renderer's eye radius inside it (178 px), so every R-relative size keeps
    // the proportion it has on the hardware.
    const CR = this.panelRadius();
    const R = CR * EYE_RADIUS_RATIO;
    const gap = CR * 2.7;
    const cy = H * 0.46;
    for (const side of [-1, 1]) {
      const ex = W / 2 + (side * gap) / 2;
      ctx.save();
      ctx.beginPath();
      ctx.arc(ex, cy, CR, 0, 7);
      ctx.clip();
      // The eye interior stays black regardless of the page background.
      ctx.fillStyle = '#000';
      ctx.fillRect(ex - CR, cy - CR, CR * 2, CR * 2);
      this.drawEye(ctx, ex, cy, R, side, this.blinkEyes & (side < 0 ? 1 : 2) ? blink : 0);
      this.drawZzz(ctx, side, ex, cy, R, R);
      ctx.restore();
      // bezel ring
      ctx.strokeStyle = 'rgba(60,70,85,.45)';
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.arc(ex, cy, CR, 0, 7);
      ctx.stroke();
    }
  }

  /* Lid coverage path shared by lid fill and the Zzz clip. */
  private makeLidPath(E: number, lidT: number, lidB: number, slant: number, botCurve: number): Path2D {
    const path = new Path2D();
    const topEdge = -E + lidT * E * 2;
    const topLeft = topEdge - slant * E * 0.35;
    const topRight = topEdge + slant * E * 0.35;
    const topBulge = E * 0.16 * clamp(lidT * 2, 0, 1);

    path.moveTo(-E * 1.3, -E * 1.3);
    path.lineTo(E * 1.3, -E * 1.3);
    path.lineTo(E * 1.3, topRight);
    path.bezierCurveTo(E * 0.4, topRight + topBulge, -E * 0.4, topLeft + topBulge, -E * 1.3, topLeft);
    path.closePath();

    const bottomEdge = E - lidB * E * 2;
    const bottomBulge = -botCurve * E * 0.55 - E * 0.16 * clamp(lidB * 2, 0, 1);
    // Same winding as the top lid so the overlap is a union.
    path.moveTo(-E * 1.3, bottomEdge);
    path.bezierCurveTo(-E * 0.45, bottomEdge + bottomBulge, E * 0.45, bottomEdge + bottomBulge, E * 1.3, bottomEdge);
    path.lineTo(E * 1.3, E * 1.3);
    path.lineTo(-E * 1.3, E * 1.3);
    path.closePath();
    return path;
  }

  private drawEye(ctx: CanvasRenderingContext2D, cx: number, cy: number, R: number, side: number, blink: number): void {
    const S = this.cur;
    const irisHex = side < 0 ? this.irisHexL : this.irisHexR;
    const IC = hexRgb(irisHex);
    const squishY = 1 - S.squint * 0.25;
    const gx = S.gazeX * R * 0.30 + side * S.derp * R * 0.24;
    const gy = S.gazeY * R * 0.26 + side * S.derp * R * 0.11 + S.derp * R * 0.06;
    const minimalSceneExit =
      this.sceneType && this.sceneStyle === 'minimal' && this.scenePhase === 'reopening-minimal-out';

    ctx.save();
    ctx.translate(cx, cy);

    if (this.sceneType && !minimalSceneExit) {
      if (this.sceneStyle === 'minimal') {
        ctx.fillStyle = '#000';
        ctx.beginPath();
        ctx.arc(0, 0, R * 1.08, 0, 7);
        ctx.fill();
        if (this.scenePrevType) {
          // The outgoing scene keeps its own payload during the cross-fade.
          const live = this.scenePayload;
          this.scenePayload = this.scenePrevPayload ?? live;
          this.drawScene(ctx, R * 1.08, IC, side, this.scenePrevType, 1 - this.sceneFade, true, this.scenePrevStartedAt);
          this.scenePayload = live;
        }
        this.drawScene(ctx, R * 1.08, IC, side, this.sceneType, this.sceneFade, true, this.sceneStartedAt);
      } else {
        this.drawScene(ctx, R * 1.08, IC, side, this.sceneType, 1, false, this.sceneStartedAt);
        ctx.fillStyle = '#000';
        ctx.fill(this.makeLidPath(R * 1.05, 0.86 * this.sceneLid, 0.14 * this.sceneLid, 0, 0));
      }
      ctx.restore();
      return;
    }

    // Base = outer glow + iris disc. The device bakes both once (bake_base)
    // and only recolours them per frame (bake_base_composite), so they ignore
    // squint, the dizzy wobble, irisScale and gaze: the globe never moves.
    const G = R * GLOBE_RATIO;
    let g = ctx.createRadialGradient(0, 0, 0, 0, 0, R);
    g.addColorStop(0, rgba(IC, clamp(S.glow, 0, 1) * 0.175));
    g.addColorStop(1, rgba(IC, 0));
    ctx.fillStyle = g;
    ctx.beginPath();
    ctx.arc(0, 0, R, 0, 7);
    ctx.fill();

    g = ctx.createRadialGradient(0, 0, 0, 0, 0, G);
    for (const [at, color] of irisStops(IC)) g.addColorStop(at, color);
    ctx.fillStyle = g;
    ctx.beginPath();
    ctx.arc(0, 0, G, 0, 7);
    ctx.fill();

    // Features follow the eye transform (prepare(): squint + dizzy rotation).
    if (this.mode === 'dizzy') ctx.rotate(Math.sin(this.tNow * 3 + side) * 0.06);
    ctx.scale(1, squishY);

    const IR = R * S.irisScale;
    const px1 = R * DEVICE_PX;

    // Iris fibers, iris(): 48 lines one DEVICE pixel wide, endpoints kept
    // inside the globe. A fixed 1 CSS px stroke was ~3x too heavy on a phone
    // preview and visibly brightened the iris.
    ctx.globalAlpha = 0.16;
    ctx.strokeStyle = rgba(shade(IC, 1.6), 1);
    ctx.lineWidth = px1;
    for (let i = 0; i < 48; i++) {
      const a = (i / 48) * Math.PI * 2 + Math.sin(i * 7) * 0.1;
      const r1 = IR * (0.28 + (((i * 37) % 13) / 13) * 0.15);
      const r2 = IR * (0.82 + (((i * 53) % 7) / 7) * 0.14);
      const from = clampToGlobe(gx + Math.cos(a) * r1, gy + Math.sin(a) * r1, G - px1);
      const to = clampToGlobe(gx * 0.3 + Math.cos(a) * r2, gy * 0.3 + Math.sin(a) * r2, G - px1);
      ctx.beginPath();
      ctx.moveTo(from[0], from[1]);
      ctx.lineTo(to[0], to[1]);
      ctx.stroke();
    }
    ctx.globalAlpha = 1;

    // Pupil (lens shape: slit <-> circle), pupil(): the centre is pulled back
    // so the whole outline stays inside the globe.
    const pw = S.pupilW, ph = S.pupilH;
    const w = IR * pw * 0.80, h = IR * ph * 0.84;
    if (w > 0.01 * px1 && h > 0.01 * px1) {
      const [pcx, pcy] = clampToGlobe(gx, gy, G - (Math.max(w, h) + px1));
      ctx.save();
      ctx.translate(pcx, pcy);
      g = ctx.createRadialGradient(0, 0, 0, 0, 0, Math.max(w, h));
      g.addColorStop(0, '#101216');
      g.addColorStop(0.8, '#05070a');
      g.addColorStop(1, '#000');
      ctx.fillStyle = g;
      ctx.beginPath();
      ctx.ellipse(0, 0, w, h, 0, 0, 7);
      ctx.fill();
      ctx.strokeStyle = rgba(shade(IC, 1.7), 0.5);
      ctx.lineWidth = IR * 0.02;
      ctx.stroke();
      ctx.restore();
    }

    if (S.overlay > 0.02) this.drawOverlay(ctx, IR, gx, gy, IC, S.overlay, side);

    // Highlights (two dots, counter-moving with gaze)
    ctx.globalAlpha = 0.9;
    ctx.fillStyle = '#fff';
    ctx.beginPath();
    ctx.ellipse(-IR * 0.33 + gx * 0.55, -IR * 0.38 + gy * 0.55, IR * 0.13, IR * 0.10, -0.5, 0, 7);
    ctx.fill();
    ctx.globalAlpha = 0.45;
    ctx.beginPath();
    ctx.arc(IR * 0.30 + gx * 0.6, IR * 0.24 + gy * 0.6, IR * 0.05, 0, 7);
    ctx.fill();
    ctx.globalAlpha = 1;

    // Lids. Blink first (nyabula_eye_engine_apply_blink), then the scene lid
    // pulls whatever is left towards the closed pose (prepare_scene_lids).
    const blinkBotTarget = Math.max(0.14, S.lidBot);
    const blinkTopTarget = Math.max(S.lidTop, 1 - blinkBotTarget);
    const lid = this.sceneLid;
    const lidT = clamp(lerp(lerp(S.lidTop, blinkTopTarget, blink), 0.86, lid), 0, 1);
    const lidB = clamp(lerp(lerp(S.lidBot, blinkBotTarget, blink), 0.14, lid), 0, 1);
    const open = (1 - blink) * (1 - lid);
    ctx.fillStyle = '#000';
    const E = IR * 1.04;
    const slant = S.lidSlant * side * -1 * open;
    const lidPath = this.makeLidPath(E, lidT, lidB, slant, S.botCurve * open);
    ctx.fill(lidPath);
    if (minimalSceneExit) {
      ctx.save();
      ctx.clip(lidPath);
      this.drawScene(ctx, R * 1.08, IC, side, this.sceneType!, this.sceneFade, true, this.sceneStartedAt);
      ctx.restore();
    }
    ctx.restore();
  }

  private drawZzz(ctx: CanvasRenderingContext2D, side: number, ex: number, cy: number, R: number, CR: number): void {
    const S = this.cur;
    const IR = R * S.irisScale;
    const E = IR * 1.04;
    const lidT = this.sleepEndTop * this.zzzMask;
    const lidB = this.sleepEndBot * this.zzzMask;

    ctx.save();
    ctx.translate(ex, cy);
    ctx.scale(1, 1 - S.squint * 0.25);
    ctx.clip(this.makeLidPath(E, lidT, lidB, 0, 0));
    ctx.globalAlpha *= sceneEase(this.zzzMask);

    for (const z of this.zzz) {
      if (z.side !== side) continue;
      const p = clamp((CR * 1.1 - z.y) / (CR * 2.2), 0, 1);
      const a = Math.sin(p * Math.PI);
      ctx.save();
      ctx.translate(z.ox + Math.sin(z.life * 2 + z.ph) * z.sway, z.y);
      ctx.rotate(z.rot + Math.sin(z.life * 1.5 + z.ph) * 0.15);
      ctx.globalAlpha = a * 0.85;
      ctx.fillStyle = 'rgba(165,195,255,1)';
      ctx.font = fontEnglish(z.size * (1 + p * 0.5));
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText('Z', 0, 0);
      ctx.restore();
    }
    ctx.restore();
  }

  /* ---------- expression overlays ---------- */
  private drawOverlay(ctx: CanvasRenderingContext2D, IR: number, gx: number, gy: number, IC: RGB, alpha: number, side: number): void {
    const star = (r: number, rot: number) => {
      ctx.beginPath();
      for (let i = 0; i < 10; i++) {
        const a = rot + (i * Math.PI) / 5;
        const rr = i % 2 ? r * 0.45 : r;
        if (i) ctx.lineTo(Math.cos(a) * rr, Math.sin(a) * rr);
        else ctx.moveTo(Math.cos(a) * rr, Math.sin(a) * rr);
      }
      ctx.closePath();
    };
    const heart = (r: number) => {
      ctx.beginPath();
      HEART_POINTS.forEach(([x, y], i) => (i ? ctx.lineTo(x * r, y * r) : ctx.moveTo(x * r, y * r)));
      ctx.closePath();
    };
    const tNow = this.tNow;
    ctx.save();
    ctx.translate(gx, gy);
    ctx.globalAlpha = alpha;
    if (this.mode === 'processing') {
      const rot = tNow * 2.6;
      const BC = shade(IC, 1.9);
      ctx.lineCap = 'round';
      for (let i = 0; i < 14; i++) {
        const a0 = rot - i * 0.16;
        const seg = 0.17;
        ctx.strokeStyle = rgba(BC, Math.pow(1 - i / 14, 1.6) * 0.95);
        ctx.lineWidth = IR * 0.055 * (1 - (i / 14) * 0.6);
        ctx.beginPath();
        ctx.arc(0, 0, IR * 0.46, a0 - seg, a0 + 0.02);
        ctx.stroke();
      }
      // overlays(): flat fills only -- the device rasteriser has no blur, so
      // none of the overlays carries a bloom.
      ctx.fillStyle = '#fff';
      ctx.beginPath();
      ctx.arc(Math.cos(rot) * IR * 0.46, Math.sin(rot) * IR * 0.46, IR * 0.05, 0, 7);
      ctx.fill();
      ctx.globalAlpha = alpha * 0.35;
      ctx.strokeStyle = rgba(BC, 1);
      ctx.lineWidth = IR * 0.02;
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.62, -rot * 0.5, -rot * 0.5 + 2.4);
      ctx.stroke();
      ctx.globalAlpha = alpha;
      const br = 0.7 + 0.3 * Math.sin(tNow * 4);
      ctx.fillStyle = rgba(BC, 0.55 + br * 0.4);
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.055 * (1 + br * 0.5), 0, 7);
      ctx.fill();
      // Ripple: full expansion, 200 ms rest, restart.
      const rippleDuration = 1 / 0.9;
      const ripplePause = 0.2;
      const rippleTime = this.modeT % (rippleDuration + ripplePause);
      if (rippleTime < rippleDuration) {
        const rp = rippleTime / rippleDuration;
        ctx.strokeStyle = rgba(BC, (1 - rp) * 0.5);
        ctx.lineWidth = IR * 0.02;
        ctx.beginPath();
        ctx.arc(0, 0, IR * 0.08 + rp * IR * 0.3, 0, 7);
        ctx.stroke();
      }
    } else if (this.mode === 'star') {
      const pulse = 1 + Math.sin(tNow * 5) * 0.12;
      const rot = Math.sin(tNow * 1.5) * 0.25;
      ctx.fillStyle = '#ffd94d';
      star(IR * 0.5 * pulse, rot - Math.PI / 2);
      ctx.fill();
      for (let i = 0; i < 2; i++) {
        const a = tNow * 2 + i * 3 + side;
        const r = IR * 0.75;
        ctx.globalAlpha = alpha * (0.4 + 0.3 * Math.sin(tNow * 4 + i * 2));
        ctx.fillStyle = '#fff3b8';
        star(IR * 0.09, a);
        ctx.save();
        ctx.translate(Math.cos(a) * r, Math.sin(a) * r * 0.8);
        star(IR * 0.09, a);
        ctx.fill();
        ctx.restore();
      }
    } else if (this.mode === 'heart') {
      const beat = (tNow * 1.6) % 1;
      // overlays(): one beat per cycle; the demo's softer second beat is not
      // on the device.
      const pulse = 1 + (beat < 0.15 ? Math.sin((beat / 0.15) * Math.PI) * 0.18 : 0);
      ctx.fillStyle = '#ff4d79';
      heart(IR * 0.5 * pulse);
      ctx.fill();
    } else if (this.mode === 'dizzy') {
      const rot = tNow * 4 * (side < 0 ? 1 : -1);
      ctx.strokeStyle = '#0a0c10';
      ctx.lineWidth = IR * 0.09;
      ctx.lineCap = 'round';
      ctx.beginPath();
      for (let a = 0; a < Math.PI * 5; a += 0.15) {
        const r = (IR * 0.55 * a) / (Math.PI * 5);
        const x = Math.cos(a + rot) * r;
        const y = Math.sin(a + rot) * r;
        if (a) ctx.lineTo(x, y);
        else ctx.moveTo(x, y);
      }
      ctx.stroke();
    } else if (this.mode === 'sad') {
      ctx.globalAlpha = alpha * (0.5 + 0.2 * Math.sin(tNow * 2));
      const g2 = ctx.createLinearGradient(0, IR * 0.3, 0, IR);
      g2.addColorStop(0, 'rgba(180,220,255,0)');
      g2.addColorStop(1, 'rgba(190,225,255,.55)');
      ctx.fillStyle = g2;
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.98, 0, 7);
      ctx.fill();
    }
    ctx.restore();
  }

  /* ---------- scene drawing ---------- */

  private materialPath(name: string): Path2D {
    let p = this.materialPathCache.get(name);
    if (!p) {
      const icon = MATERIAL_ICONS[name];
      if (!icon) throw new Error(`Missing extracted SVG icon: ${name}`);
      p = new Path2D(icon.path);
      this.materialPathCache.set(name, p);
    }
    return p;
  }

  private materialPathLength(name: string): number {
    let len = this.materialLengthCache.get(name);
    if (len === undefined) {
      if (typeof document !== 'undefined') {
        const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
        path.setAttribute('d', MATERIAL_ICONS[name].path);
        len = (path as SVGPathElement).getTotalLength();
      } else {
        len = 1000;
      }
      this.materialLengthCache.set(name, len);
    }
    return len;
  }

  private iconReveal(startedAt = this.sceneStartedAt): number {
    const start = Math.max(startedAt, this.optionChangedAt);
    return sceneEase(clamp((this.tNow - start) / 0.72, 0, 1));
  }

  private iconSceneAlpha(): number {
    if (!this.scenePhase.startsWith('closing')) return 1;
    return 1 - sceneEase(clamp(this.scenePhaseT / SCENE_CLOSE_TIME, 0, 1));
  }

  private drawMaterialIcon(
    ctx: CanvasRenderingContext2D,
    name: string,
    IR: number,
    C: number[],
    {
      size = 1.18,
      reveal = this.iconReveal(),
      alpha = 1,
      rotation = 0,
      sceneAlpha = this.iconSceneAlpha(),
    }: { size?: number; reveal?: number; alpha?: number; rotation?: number; sceneAlpha?: number } = {},
  ): void {
    const icon = MATERIAL_ICONS[name];
    if (!icon) return;
    const length = this.materialPathLength(name);
    const fillIn = sceneEase(clamp((reveal - 0.46) / 0.54, 0, 1));
    const scale = (IR * size) / icon.viewBox[2];
    ctx.save();
    ctx.globalAlpha *= alpha * sceneAlpha;
    ctx.rotate(rotation);
    ctx.scale(scale, scale);
    ctx.translate(-icon.viewBox[2] / 2, -icon.viewBox[3] / 2);
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    ctx.lineWidth = 18;
    ctx.strokeStyle = rgba(C, lerp(0.88, 0.20, fillIn));
    ctx.setLineDash([length, length]);
    ctx.lineDashOffset = length * (1 - reveal);
    ctx.stroke(this.materialPath(name));
    if (fillIn > 0) {
      ctx.setLineDash([]);
      ctx.fillStyle = rgba(C, 0.92 * fillIn);
      ctx.fill(this.materialPath(name));
    }
    ctx.restore();
  }

  private sceneGlow(ctx: CanvasRenderingContext2D, IC: RGB, a = 1): number[] {
    const C = shade(IC, 1.75);
    ctx.shadowColor = rgba(C, 0.9 * a);
    ctx.shadowBlur = 18 * a;
    return C;
  }

  private drawScene(
    ctx: CanvasRenderingContext2D,
    IR: number,
    IC: RGB,
    side: number,
    type: string,
    alpha = 1,
    minimal = false,
    startedAt = this.sceneStartedAt,
  ): void {
    ctx.save();
    ctx.globalAlpha *= clamp(alpha, 0, 1);
    switch (type) {
      case 'music':
        this.drawMusicScene(ctx, IR, IC, side, minimal, startedAt);
        break;
      case 'timer':
        this.drawTimerScene(ctx, IR, IC, side, minimal, startedAt);
        break;
      case 'weather':
        this.drawWeatherScene(ctx, IR, IC, side, minimal);
        break;
      case 'battery':
        this.drawBatteryScene(ctx, IR, IC, side, minimal);
        break;
      case 'pairing':
        this.drawPairingScene(ctx, IR, IC, side, minimal);
        break;
      case 'alarm':
        this.drawAlarmScene(ctx, IR, IC, side, minimal);
        break;
      case 'call':
        this.drawCallScene(ctx, IR, IC, side, minimal);
        break;
      case 'task':
        this.drawTaskScene(ctx, IR, IC, side, minimal);
        break;
      case 'stopwatch':
        this.drawStopwatchScene(ctx, IR, IC, side, minimal, startedAt);
        break;
      case 'calendar':
        this.drawCalendarScene(ctx, IR, IC, side, minimal);
        break;
      case 'sleep-timer':
        this.drawSleepTimerScene(ctx, IR, IC, side, minimal, startedAt);
        break;
      case 'network':
        this.drawNetworkScene(ctx, IR, IC, side, minimal);
        break;
      case 'audio':
        this.drawAudioScene(ctx, IR, IC, side, minimal);
        break;
      case 'eq':
        this.drawEqScene(ctx, IR, IC, side, minimal, startedAt);
        break;
      case 'caption':
        this.drawCaptionScene(ctx, IR, IC, side, minimal);
        break;
      case 'briefing':
        this.drawBriefingScene(ctx, IR, IC, side, minimal);
        break;
      case 'privacy':
        this.drawPrivacyScene(ctx, IR, IC, side, minimal);
        break;
      case 'identity':
        this.drawIdentityScene(ctx, IR, IC, side, minimal);
        break;
      case 'memory':
        this.drawMemoryScene(ctx, IR, IC, side, minimal);
        break;
      case 'devices':
        this.drawDevicesScene(ctx, IR, IC, side, minimal);
        break;
      case 'system':
        this.drawSystemScene(ctx, IR, IC, side, minimal);
        break;
      case 'health':
        this.drawHealthScene(ctx, IR, IC, side, minimal);
        break;
      case 'presence':
        this.drawPresenceScene(ctx, IR, IC, side, minimal);
        break;
      case 'companion':
        this.drawCompanionScene(ctx, IR, IC, side, minimal);
        break;
      case 'home':
        this.drawCatHomeScene(ctx, IR, IC, side, minimal);
        break;
      case 'subwoofer':
        this.drawSubwooferScene(ctx, IR, IC, side, minimal);
        break;
      default:
        this.drawFallbackScene(ctx, IR, IC, side, type, minimal);
        break;
    }
    ctx.restore();
  }

  private drawFallbackScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, type: string, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.5);
    if (!minimal) {
      ctx.fillStyle = 'rgba(0,3,7,.68)';
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.93, 0, 7);
      ctx.fill();
    }
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.strokeStyle = rgba(C, 0.22);
    ctx.lineWidth = IR * 0.02;
    ctx.beginPath();
    ctx.arc(0, 0, IR * 0.7, 0, 7);
    ctx.stroke();
    ctx.fillStyle = rgba(C, 0.9);
    ctx.font = fontMixed(IR * 0.14);
    ctx.fillText(type, 0, -IR * 0.05);
    ctx.fillStyle = rgba(C, 0.4);
    ctx.font = fontBody(IR * 0.08);
    ctx.fillText(side < 0 ? '场景移植中' : 'coming soon', 0, IR * 0.22);
  }

  private drawMusicPlayback(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, C: number[], startedAt: number): void {
    const duration = 235;
    const elapsed = (this.tNow - startedAt) % duration;
    const progress = elapsed / duration;
    const cover = ctx.createRadialGradient(-IR * 0.22, -IR * 0.30, IR * 0.03, 0, 0, IR * 0.92);
    cover.addColorStop(0, rgba(shade(IC, 1.35), 0.18));
    cover.addColorStop(0.45, rgba(IC, 0.08));
    cover.addColorStop(1, 'rgba(0,0,0,0)');
    ctx.fillStyle = cover;
    ctx.beginPath();
    ctx.arc(0, 0, IR * 0.92, 0, 7);
    ctx.fill();
    ctx.lineCap = 'round';
    ctx.lineWidth = IR * 0.032;
    ctx.strokeStyle = rgba(C, 0.16);
    ctx.beginPath();
    ctx.arc(0, 0, IR * 0.89, -Math.PI / 2, Math.PI * 1.5);
    ctx.stroke();
    ctx.strokeStyle = rgba(C, 0.92);
    ctx.beginPath();
    ctx.arc(0, 0, IR * 0.89, -Math.PI / 2, -Math.PI / 2 + progress * Math.PI * 2);
    ctx.stroke();
    const a = -Math.PI / 2 + progress * Math.PI * 2;
    ctx.fillStyle = rgba(C, 0.95);
    ctx.beginPath();
    ctx.arc(Math.cos(a) * IR * 0.89, Math.sin(a) * IR * 0.89, IR * 0.024, 0, 7);
    ctx.fill();
    ctx.save();
    ctx.translate(0, -IR * 0.08);
    ctx.fillStyle = rgba(C, 0.94);
    ctx.beginPath();
    ctx.moveTo(-IR * 0.105, -IR * 0.19);
    ctx.lineTo(IR * 0.205, 0);
    ctx.lineTo(-IR * 0.105, IR * 0.19);
    ctx.closePath();
    ctx.fill();
    ctx.restore();
    const em = String(Math.floor(elapsed / 60)).padStart(2, '0');
    const es = String(Math.floor(elapsed % 60)).padStart(2, '0');
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.font = fontEnglish(IR * 0.080);
    ctx.fillStyle = rgba(C, 0.50);
    ctx.fillText('NYABULA MIX', 0, IR * 0.36);
    ctx.font = fontEnglish(IR * 0.090);
    ctx.fillStyle = rgba(C, 0.76);
    ctx.fillText(`${em}:${es}  /  03:55`, 0, IR * 0.49);
  }

  private drawMusicSpectrum(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, C: number[], alpha: number): void {
    ctx.save();
    ctx.globalAlpha *= alpha;
    ctx.translate(0, IR * 0.10);
    const bars = 9, bw = IR * 0.062, gap = IR * 0.045;
    ctx.fillStyle = rgba(C, 0.32);
    ctx.font = fontTitle(IR * 0.080);
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText('频谱', 0, -IR * 0.48);
    for (let i = 0; i < bars; i++) {
      const wave = 0.5 + 0.5 * Math.sin(this.tNow * (3.4 + (i % 3) * 0.24) + i * 0.88);
      const envelope = 0.58 + 0.42 * Math.sin(((i + 1) / (bars + 1)) * Math.PI);
      const h = IR * (0.13 + 0.49 * wave * envelope);
      const x = (i - (bars - 1) / 2) * (bw + gap);
      const grad = ctx.createLinearGradient(0, IR * 0.31, 0, -IR * 0.31);
      grad.addColorStop(0, rgba(shade(IC, 0.70), 0.48));
      grad.addColorStop(1, rgba(C, 0.94));
      ctx.fillStyle = grad;
      ctx.beginPath();
      ctx.roundRect(x - bw / 2, -h / 2, bw, h, bw / 2);
      ctx.fill();
    }
    ctx.restore();
  }

  private drawMusicLyrics(ctx: CanvasRenderingContext2D, IR: number, C: number[], alpha: number): void {
    ctx.save();
    ctx.globalAlpha *= alpha;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.translate(0, IR * 0.08);
    ctx.font = fontBody(IR * 0.095);
    ctx.fillStyle = rgba(C, 0.22);
    ctx.fillText('灯火落进夜里', 0, -IR * 0.30);
    ctx.font = fontTitle(IR * 0.155);
    ctx.fillStyle = rgba(C, 0.94);
    ctx.fillText('我听见你', 0, -IR * 0.03);
    ctx.font = fontBody(IR * 0.095);
    ctx.fillStyle = rgba(C, 0.30);
    ctx.fillText('轻轻回应', 0, IR * 0.27);
    ctx.restore();
  }

  private drawMusicScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean, startedAt: number): void {
    const C = this.sceneGlow(ctx, IC, 0.65);
    if (!minimal) {
      ctx.fillStyle = 'rgba(0,3,7,.68)';
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.93, 0, 7);
      ctx.fill();
    }
    ctx.shadowBlur = 0;
    if (side < 0) this.drawMusicPlayback(ctx, IR, IC, C, startedAt);
    else {
      if (this.musicPrevView) {
        if (this.musicPrevView === 'spectrum') this.drawMusicSpectrum(ctx, IR, IC, C, 1 - this.musicViewFade);
        else this.drawMusicLyrics(ctx, IR, C, 1 - this.musicViewFade);
      }
      if (this.musicView === 'spectrum') this.drawMusicSpectrum(ctx, IR, IC, C, this.musicViewFade);
      else this.drawMusicLyrics(ctx, IR, C, this.musicViewFade);
    }
    ctx.shadowBlur = 0;
  }

  private drawTimerScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean, startedAt: number): void {
    const C = this.sceneGlow(ctx, IC, 0.65);
    if (!minimal) {
      ctx.fillStyle = 'rgba(0,3,7,.72)';
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.93, 0, 7);
      ctx.fill();
    }
    ctx.shadowBlur = 0;
    const payload = this.scenePayload;
    const total = typeof payload.duration_ms === 'number' ? payload.duration_ms / 1000
      : typeof payload.total_ms === 'number' ? payload.total_ms / 1000
      : typeof payload.seconds === 'number' ? payload.seconds : 300;
    const initial = typeof payload.remaining_ms === 'number' ? payload.remaining_ms / 1000 : total;
    const active = payload.active ?? payload.running ?? true;
    const precise = Math.max(0, initial - (active ? this.tNow - startedAt : 0));
    const remaining = Math.ceil(precise);
    const mm = String(Math.floor(remaining / 60)).padStart(2, '0');
    const ss = String(remaining % 60).padStart(2, '0');
    const value = side < 0 ? mm : ss;
    const urgency = remaining <= 10 ? 0.82 + Math.sin(this.tNow * 8) * 0.18 : 1;
    ctx.fillStyle = rgba(C, 0.96 * urgency);
    ctx.font = fontEnglish(IR * 0.62);
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText(value, 0, -IR * 0.02);
    ctx.font = fontBody(IR * 0.085);
    ctx.fillStyle = rgba(C, 0.42);
    ctx.fillText(side < 0 ? '分钟' : '秒钟', 0, IR * 0.34);
    ctx.lineCap = 'round';
    ctx.lineWidth = IR * 0.035;
    ctx.strokeStyle = rgba(C, 0.22);
    ctx.beginPath();
    ctx.arc(0, 0, IR * 0.72, -Math.PI / 2, Math.PI * 1.5);
    ctx.stroke();
    const p = total > 0 ? clamp(precise / total, 0, 1) : 0;
    ctx.strokeStyle = rgba(C, 0.82);
    ctx.beginPath();
    ctx.arc(0, 0, IR * 0.72, -Math.PI / 2, -Math.PI / 2 + p * Math.PI * 2);
    ctx.stroke();
    ctx.strokeStyle = rgba(C, 0.18);
    ctx.lineWidth = IR * 0.011;
    for (let i = 0; i < 30; i++) {
      const a = (i * Math.PI) / 15;
      const inner = i % 5 === 0 ? 0.61 : 0.64;
      ctx.beginPath();
      ctx.moveTo(Math.cos(a) * IR * inner, Math.sin(a) * IR * inner);
      ctx.lineTo(Math.cos(a) * IR * 0.67, Math.sin(a) * IR * 0.67);
      ctx.stroke();
    }
    ctx.shadowBlur = 0;
  }

  /* ----- weather ----- */
  private drawRainDrop(ctx: CanvasRenderingContext2D, x: number, y: number, size: number, C: number[], alpha: number, tilt = -0.12): void {
    ctx.save();
    ctx.translate(x, y);
    ctx.rotate(tilt);
    ctx.globalAlpha *= alpha;
    ctx.fillStyle = rgba(C, 0.95);
    ctx.beginPath();
    ctx.moveTo(0, -size * 0.72);
    ctx.bezierCurveTo(size * 0.08, -size * 0.46, size * 0.32, -size * 0.12, size * 0.32, size * 0.10);
    ctx.bezierCurveTo(size * 0.32, size * 0.40, size * 0.17, size * 0.58, 0, size * 0.58);
    ctx.bezierCurveTo(-size * 0.17, size * 0.58, -size * 0.32, size * 0.40, -size * 0.32, size * 0.10);
    ctx.bezierCurveTo(-size * 0.32, -size * 0.12, -size * 0.08, -size * 0.46, 0, -size * 0.72);
    ctx.fill();
    ctx.restore();
  }

  private makeCloudPath(IR: number, dx = 0, dy = 0): Path2D {
    const p = new Path2D();
    p.moveTo(dx - IR * 0.46, dy + IR * 0.18);
    p.bezierCurveTo(dx - IR * 0.49, dy + IR * 0.05, dx - IR * 0.43, dy - IR * 0.15, dx - IR * 0.24, dy - IR * 0.18);
    p.bezierCurveTo(dx - IR * 0.18, dy - IR * 0.43, dx + IR * 0.13, dy - IR * 0.48, dx + IR * 0.24, dy - IR * 0.22);
    p.bezierCurveTo(dx + IR * 0.40, dy - IR * 0.21, dx + IR * 0.49, dy - IR * 0.08, dx + IR * 0.48, dy + IR * 0.09);
    p.bezierCurveTo(dx + IR * 0.48, dy + IR * 0.18, dx + IR * 0.41, dy + IR * 0.22, dx + IR * 0.30, dy + IR * 0.22);
    p.lineTo(dx - IR * 0.39, dy + IR * 0.22);
    p.bezierCurveTo(dx - IR * 0.43, dy + IR * 0.22, dx - IR * 0.46, dy + IR * 0.21, dx - IR * 0.46, dy + IR * 0.18);
    p.closePath();
    return p;
  }

  private drawCloudBody(ctx: CanvasRenderingContext2D, IR: number, C: number[], drift = 0, scale = 1): void {
    ctx.save();
    ctx.scale(scale, scale);
    ctx.fillStyle = rgba(shade(C, 0.38), 0.55);
    ctx.fill(this.makeCloudPath(IR, drift + IR * 0.035, IR * 0.035));
    const cloud = ctx.createLinearGradient(0, -IR * 0.42, 0, IR * 0.24);
    cloud.addColorStop(0, rgba(shade(C, 1.06), 0.98));
    cloud.addColorStop(0.72, rgba(C, 0.91));
    cloud.addColorStop(1, rgba(shade(C, 0.72), 0.92));
    ctx.fillStyle = cloud;
    ctx.fill(this.makeCloudPath(IR, drift, 0));
    ctx.strokeStyle = rgba(shade(C, 1.22), 0.28);
    ctx.lineWidth = IR * 0.014;
    ctx.stroke(this.makeCloudPath(IR, drift, -IR * 0.008));
    ctx.restore();
  }

  private drawRainWeather(ctx: CanvasRenderingContext2D, IR: number, C: number[], storm = false): void {
    const tNow = this.tNow;
    const drift = Math.sin(tNow * 0.72) * IR * 0.025;
    const dropXs = storm ? [-0.39, -0.27, -0.14, 0, 0.13, 0.26, 0.39] : [-0.34, -0.18, -0.02, 0.15, 0.31];
    for (let i = 0; i < dropXs.length; i++) {
      const p = (tNow * ((storm ? 0.78 : 0.48) + i * 0.018) + i * 0.193) % 1;
      const y = IR * (-0.10 + Math.pow(p, 1.55) * 0.86);
      const fadeIn = sceneEase(clamp(p / 0.12, 0, 1));
      const fadeOut = 1 - sceneEase(clamp((y / IR - 0.64) / 0.08, 0, 1));
      this.drawRainDrop(ctx, IR * dropXs[i] + drift, y, IR * (storm ? 0.042 : 0.048), C,
        fadeIn * fadeOut * (0.60 + (i % 2) * 0.22), storm ? -0.18 : -0.12);
      if (y >= IR * 0.72) {
        const q = clamp((y / IR - 0.72) / 0.04, 0, 1);
        ctx.strokeStyle = rgba(C, (1 - q) * 0.28);
        ctx.lineWidth = IR * 0.012;
        ctx.beginPath();
        ctx.ellipse(IR * dropXs[i] + drift, IR * 0.74, IR * (0.018 + q * 0.08), IR * (0.006 + q * 0.018), 0, 0, 7);
        ctx.stroke();
      }
    }
    if (storm) {
      const flashT = (tNow + 1.37) % 6.4;
      if (flashT < 0.30) {
        const p = flashT / 0.30;
        const alpha = Math.sin(p * Math.PI);
        ctx.save();
        ctx.strokeStyle = rgba(shade(C, 1.42), alpha * 0.94);
        ctx.shadowColor = rgba(shade(C, 1.55), alpha);
        ctx.shadowBlur = IR * 0.10;
        ctx.lineWidth = IR * 0.038;
        ctx.lineCap = 'round';
        ctx.beginPath();
        ctx.moveTo(-IR * 0.07, -IR * 0.13);
        ctx.bezierCurveTo(IR * 0.10, IR * 0.04, -IR * 0.10, IR * 0.20, IR * 0.02, IR * 0.34);
        ctx.bezierCurveTo(IR * 0.15, IR * 0.47, -IR * 0.02, IR * 0.57, IR * 0.08, IR * 0.68);
        ctx.stroke();
        ctx.restore();
      }
    }
    this.drawCloudBody(ctx, IR, C, drift, storm ? 0.96 : 1);
  }

  private drawSunnyWeather(ctx: CanvasRenderingContext2D, IR: number, C: number[]): void {
    const tNow = this.tNow;
    const pulse = 1 + Math.sin(tNow * 1.2) * 0.025;
    ctx.save();
    ctx.rotate(tNow * 0.06);
    ctx.strokeStyle = rgba(C, 0.42);
    ctx.lineCap = 'round';
    ctx.lineWidth = IR * 0.026;
    for (let i = 0; i < 12; i++) {
      const a = (i * Math.PI) / 6;
      ctx.beginPath();
      ctx.moveTo(Math.cos(a) * IR * 0.48, Math.sin(a) * IR * 0.48);
      ctx.lineTo(Math.cos(a) * IR * 0.62, Math.sin(a) * IR * 0.62);
      ctx.stroke();
    }
    ctx.restore();
    const sun = ctx.createRadialGradient(-IR * 0.12, -IR * 0.15, 0, 0, 0, IR * 0.40);
    sun.addColorStop(0, rgba(shade(C, 1.28), 1));
    sun.addColorStop(1, rgba(C, 0.70));
    ctx.fillStyle = sun;
    ctx.beginPath();
    ctx.arc(0, 0, IR * 0.36 * pulse, 0, 7);
    ctx.fill();
  }

  private drawCloudyWeather(ctx: CanvasRenderingContext2D, IR: number, C: number[]): void {
    const a = Math.sin(this.tNow * 0.35) * IR * 0.035;
    ctx.save();
    ctx.globalAlpha = 0.34;
    ctx.translate(-IR * 0.17, -IR * 0.18);
    ctx.scale(0.72, 0.72);
    this.drawCloudBody(ctx, IR, shade(C, 0.65), -a);
    ctx.restore();
    ctx.save();
    ctx.translate(IR * 0.08, IR * 0.12);
    this.drawCloudBody(ctx, IR, C, a);
    ctx.restore();
  }

  private drawSnowWeather(ctx: CanvasRenderingContext2D, IR: number, C: number[]): void {
    const tNow = this.tNow;
    for (let i = 0; i < 12; i++) {
      const p = (tNow * (0.16 + (i % 3) * 0.018) + i * 0.083) % 1;
      const x = IR * (-0.46 + ((i * 0.37) % 1) * 0.92) + Math.sin(tNow * 1.3 + i) * IR * 0.035;
      const y = IR * (-0.12 + p * 0.90);
      const a = sceneEase(clamp(p / 0.12, 0, 1)) * (1 - sceneEase(clamp((p - 0.84) / 0.16, 0, 1)));
      ctx.save();
      ctx.translate(x, y);
      ctx.rotate(tNow * 0.4 + i);
      ctx.strokeStyle = rgba(C, a * 0.78);
      ctx.lineWidth = IR * 0.012;
      for (let k = 0; k < 3; k++) {
        ctx.rotate(Math.PI / 3);
        ctx.beginPath();
        ctx.moveTo(-IR * 0.028, 0);
        ctx.lineTo(IR * 0.028, 0);
        ctx.stroke();
      }
      ctx.restore();
    }
    this.drawCloudBody(ctx, IR, C, Math.sin(tNow * 0.5) * IR * 0.018, 0.94);
  }

  private drawFogWeather(ctx: CanvasRenderingContext2D, IR: number, C: number[]): void {
    const tNow = this.tNow;
    ctx.globalAlpha *= 0.92;
    ctx.lineCap = 'round';
    for (let i = 0; i < 7; i++) {
      const y = IR * (-0.50 + i * 0.16);
      const shift = Math.sin(tNow * 0.35 + i * 0.9) * IR * 0.10;
      const half = IR * (0.26 + (i % 3) * 0.10);
      ctx.strokeStyle = rgba(C, 0.22 + i * 0.055);
      ctx.lineWidth = IR * (0.018 + (i % 2) * 0.008);
      ctx.beginPath();
      ctx.moveTo(-half + shift, y);
      ctx.bezierCurveTo(-half * 0.35 + shift, y - IR * 0.035, half * 0.35 + shift, y + IR * 0.035, half + shift, y);
      ctx.stroke();
    }
  }

  private drawWeatherReadout(ctx: CanvasRenderingContext2D, IR: number, C: number[], kind: string): void {
    const m = WEATHER_META[kind] ?? WEATHER_META.rain;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.lineCap = 'round';
    ctx.lineWidth = IR * 0.024;
    ctx.strokeStyle = rgba(C, 0.16);
    ctx.beginPath();
    ctx.arc(0, 0, IR * 0.70, Math.PI * 0.83, Math.PI * 2.17);
    ctx.stroke();
    ctx.strokeStyle = rgba(C, 0.76);
    ctx.beginPath();
    ctx.arc(0, 0, IR * 0.70, Math.PI * 0.83, Math.PI * 1.61);
    ctx.stroke();
    ctx.fillStyle = rgba(C, 0.98);
    ctx.font = fontEnglish(IR * 0.50);
    ctx.fillText(m[0], 0, -IR * 0.10);
    ctx.font = fontTitle(IR * 0.125);
    ctx.fillStyle = rgba(C, 0.64);
    ctx.fillText(m[1], 0, IR * 0.23);
    ctx.font = fontBody(IR * 0.072);
    ctx.fillStyle = rgba(C, 0.40);
    ctx.fillText(m[2], -IR * 0.23, IR * 0.39);
    ctx.fillText(m[4], IR * 0.23, IR * 0.39);
    ctx.font = fontEnglish(IR * 0.115);
    ctx.fillStyle = rgba(C, 0.72);
    ctx.fillText(m[3], -IR * 0.23, IR * 0.50);
    ctx.fillText(m[5], IR * 0.23, IR * 0.50);
  }

  private drawWeatherState(ctx: CanvasRenderingContext2D, IR: number, C: number[], side: number, kind: string, alpha: number): void {
    ctx.save();
    ctx.globalAlpha *= alpha;
    if (side < 0) {
      const centerY = kind === 'rain' || kind === 'storm' ? -IR * 0.09 : kind === 'snow' ? -IR * 0.06 : 0;
      ctx.translate(0, centerY);
      if (kind === 'sunny') this.drawSunnyWeather(ctx, IR, C);
      else if (kind === 'cloudy') this.drawCloudyWeather(ctx, IR, C);
      else if (kind === 'rain') this.drawRainWeather(ctx, IR, C, false);
      else if (kind === 'storm') this.drawRainWeather(ctx, IR, C, true);
      else if (kind === 'snow') this.drawSnowWeather(ctx, IR, C);
      else this.drawFogWeather(ctx, IR, C);
    } else this.drawWeatherReadout(ctx, IR, C, kind);
    ctx.restore();
  }

  private drawWeatherScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.55);
    if (!minimal) {
      ctx.fillStyle = 'rgba(0,3,7,.64)';
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.93, 0, 7);
      ctx.fill();
    }
    ctx.shadowBlur = 0;
    if (this.weatherPrevKind) this.drawWeatherState(ctx, IR, C, side, this.weatherPrevKind, 1 - this.weatherFade);
    this.drawWeatherState(ctx, IR, C, side, this.weatherKind, this.weatherFade);
  }

  private drawBatteryScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.55);
    const levels: Record<string, number> = { charging: 0.68, low: 0.09, full: 1, hot: 0.55, dock: 0.82 };
    const level = levels[this.batteryState] ?? 0.5;
    if (!minimal) {
      ctx.fillStyle = 'rgba(0,3,7,.68)';
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.93, 0, 7);
      ctx.fill();
    }
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    const iconMap: Record<string, string> = {
      charging: 'battery_charging', low: 'battery_low', full: 'battery_full',
      hot: 'battery_hot', dock: 'battery_dock',
    };
    if (side < 0) {
      ctx.lineCap = 'round';
      ctx.strokeStyle = rgba(C, 0.20);
      ctx.lineWidth = IR * 0.055;
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.58, -Math.PI / 2, Math.PI * 1.5);
      ctx.stroke();
      ctx.strokeStyle = rgba(C, 0.92);
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.58, -Math.PI / 2, -Math.PI / 2 + level * Math.PI * 2);
      ctx.stroke();
      const icon = iconMap[this.batteryState] ?? 'battery_full';
      const pulse = this.batteryState === 'charging' || this.batteryState === 'dock'
        ? 0.88 + 0.12 * Math.sin(this.tNow * 3.2) : 1;
      if (this.batteryPrevState) {
        const previous = iconMap[this.batteryPrevState] ?? 'battery_full';
        this.drawMaterialIcon(ctx, previous, IR, C, { size: 0.78, reveal: 1, alpha: 1 - sceneEase(this.optionFade) });
      }
      this.drawMaterialIcon(ctx, icon, IR, C, { size: 0.78, alpha: pulse * sceneEase(this.optionFade) });
    } else {
      ctx.globalAlpha *= sceneEase(this.optionFade);
      ctx.save();
      ctx.translate(0, -IR * 0.055);
      const meta: Record<string, string[]> = {
        charging: ['68%', '充电中', '约 2 小时 14 分'],
        low: ['9%', '电量不足', '约 18 分钟'],
        full: ['100%', '已充满', '可以出发啦'],
        hot: ['55%', '暂停充电', '温度过高'],
        dock: ['82%', '正在底座充电', '磁吸底座已连接'],
      };
      const m = meta[this.batteryState] ?? meta.full;
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontEnglish(IR * 0.50);
      ctx.fillText(m[0], 0, -IR * 0.10);
      ctx.fillStyle = rgba(C, 0.52);
      ctx.font = fontTitle(IR * 0.105);
      ctx.fillText(m[1], 0, IR * 0.22);
      ctx.fillStyle = rgba(C, 0.72);
      ctx.font = fontBody(IR * 0.095);
      ctx.fillText(m[2], 0, IR * 0.42);
      ctx.restore();
    }
  }

  /** Pairing scene: left eye shows a QR code, right eye the 6-digit code
   * (3+3, two big lines). Without a payload code, fall back to the legacy
   * split-halves rendering. Contract: Shared/protocol/nyalink.md 配对场景. */
  private drawPairingScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.6);
    if (!minimal) {
      ctx.fillStyle = 'rgba(0,3,7,.72)';
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.93, 0, 7);
      ctx.fill();
    }
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    const qr = this.pairingQr();
    if (!qr) {
      // Legacy fallback: each eye shows half of a (placeholder) code.
      const code = String(this.scenePayload.code ?? '000000').padStart(6, '0');
      const half = side < 0 ? code.slice(0, 3) : code.slice(3, 6);
      const breath = 0.72 + 0.28 * (0.5 + 0.5 * Math.sin(this.tNow * 2.0));
      ctx.lineCap = 'round';
      ctx.strokeStyle = rgba(C, 0.20 + 0.18 * breath);
      ctx.lineWidth = IR * 0.03;
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.80, 0, 7);
      ctx.stroke();
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontEnglish(IR * 0.52);
      ctx.fillText(half, 0, -IR * 0.05);
      ctx.fillStyle = rgba(C, 0.48);
      ctx.font = fontTitle(IR * 0.10);
      ctx.fillText(side < 0 ? '配对码' : '输入到手机', 0, IR * 0.36);
      return;
    }
    if (side < 0) {
      // Left eye: QR matrix fitted inside the iris circle (bright modules on
      // the dark backdrop, ~2-module quiet zone kept inside the circle).
      const quiet = 2;
      const total = qr.size + quiet * 2;
      // Largest square inscribed in the r = 0.93*IR backdrop circle.
      const sideLen = IR * 0.93 * Math.SQRT2;
      const m = sideLen / total;
      const org = -sideLen / 2 + quiet * m;
      ctx.fillStyle = rgba(C, 0.96);
      for (let r = 0; r < qr.size; r++) {
        for (let c = 0; c < qr.size; c++) {
          if (!qr.isDark(r, c)) continue;
          // Tiny overlap avoids hairline gaps between modules.
          ctx.fillRect(org + c * m, org + r * m, m + 0.35, m + 0.35);
        }
      }
    } else {
      // Right eye: 6-digit code as two big 3-digit lines.
      const code = String(this.scenePayload.code).padStart(6, '0');
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontEnglish(IR * 0.44);
      ctx.fillText(code.slice(0, 3), 0, -IR * 0.26);
      ctx.fillText(code.slice(3, 6), 0, IR * 0.18);
      ctx.fillStyle = rgba(C, 0.48);
      ctx.font = fontTitle(IR * 0.10);
      ctx.fillText('配对码', 0, IR * 0.52);
    }
  }

  /** Lazily (re)build and cache the pairing QR for the current payload. */
  private pairingQr(): PairingQr | null {
    const text = pairingQrText(this.scenePayload);
    if (!text) return null;
    if (!this.pairingQrCache || this.pairingQrCache.text !== text) {
      this.pairingQrCache = makePairingQr(this.scenePayload);
    }
    return this.pairingQrCache;
  }

  /* ----- shared scene helpers (ported 1:1 from the demo) ----- */

  private drawSceneBackdrop(ctx: CanvasRenderingContext2D, IR: number, minimal: boolean, alpha = 0.68): void {
    if (minimal) return;
    ctx.fillStyle = `rgba(0,3,7,${alpha})`;
    ctx.beginPath();
    ctx.arc(0, 0, IR * 0.93, 0, 7);
    ctx.fill();
  }

  private drawDial(ctx: CanvasRenderingContext2D, IR: number, C: number[], progress = 0.72): void {
    ctx.lineCap = 'round';
    ctx.lineWidth = IR * 0.035;
    ctx.strokeStyle = rgba(C, 0.17);
    ctx.beginPath();
    ctx.arc(0, 0, IR * 0.70, -Math.PI / 2, Math.PI * 1.5);
    ctx.stroke();
    if (progress > 0) {
      ctx.strokeStyle = rgba(C, 0.82);
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.70, -Math.PI / 2, -Math.PI / 2 + progress * Math.PI * 2);
      ctx.stroke();
    }
  }

  /* Three water rings finish at 1.4 s, followed by a true 200 ms rest. */
  private drawIconRipples(ctx: CanvasRenderingContext2D, IR: number, C: number[]): void {
    const cycle = 1.6;
    const phase = this.tNow % cycle;
    if (phase >= 1.4) return;
    ctx.save();
    ctx.lineCap = 'round';
    ctx.lineWidth = IR * 0.014;
    for (let i = 0; i < 3; i++) {
      const p = (phase - i * 0.30) / 0.80;
      if (p < 0 || p > 1) continue;
      const eased = sceneEase(p);
      const radius = IR * (0.34 + eased * 0.34);
      ctx.strokeStyle = rgba(C, Math.sin(p * Math.PI) * 0.28);
      ctx.beginPath();
      ctx.arc(0, 0, radius, 0, 7);
      ctx.stroke();
    }
    ctx.restore();
  }

  /* Lucide Static 1.31.0 icon geometry, ISC license. Paths embedded so the
   * engine has no runtime npm/CDN dependency (same as the demo). */
  private lucidePath(d: string): Path2D {
    let p = this.materialPathCache.get(`lucide:${d}`);
    if (!p) {
      p = new Path2D(d);
      this.materialPathCache.set(`lucide:${d}`, p);
    }
    return p;
  }

  private drawLucideAlarm(ctx: CanvasRenderingContext2D, IR: number, C: number[]): void {
    const s = IR * 0.040;
    const shake = Math.sin(this.tNow * 14) * IR * 0.004 * (0.5 + 0.5 * Math.sin(this.tNow * 4.8));
    this.drawIconRipples(ctx, IR, C);
    ctx.save();
    ctx.translate(shake - 12 * s, -12 * s);
    ctx.scale(s, s);
    ctx.strokeStyle = rgba(C, 0.96);
    ctx.lineWidth = 2;
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    ctx.beginPath();
    ctx.arc(12, 13, 8, 0, 7);
    ctx.stroke();
    for (const d of ['M12 9v4l2 2', 'M5 3 2 6', 'm22 6-3-3', 'M6.38 18.7 4 21', 'M17.64 18.67 20 21']) {
      ctx.stroke(this.lucidePath(d));
    }
    ctx.restore();
  }

  private drawLucidePhoneCall(ctx: CanvasRenderingContext2D, IR: number, C: number[], animated = true): void {
    const s = IR * 0.038;
    const pulse = 0.82 + 0.18 * Math.sin(this.tNow * 4.4);
    if (animated) this.drawIconRipples(ctx, IR, C);
    ctx.save();
    ctx.translate(-12 * s, -12 * s);
    ctx.scale(s, s);
    ctx.strokeStyle = rgba(C, pulse);
    ctx.shadowColor = rgba(C, 0.65);
    ctx.shadowBlur = 1.5;
    ctx.lineWidth = 2;
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    ctx.stroke(this.lucidePath('M13 2a9 9 0 0 1 9 9'));
    ctx.stroke(this.lucidePath('M13 6a5 5 0 0 1 5 5'));
    ctx.stroke(this.lucidePath('M13.832 16.568a1 1 0 0 0 1.213-.303l.355-.465A2 2 0 0 1 17 15h3a2 2 0 0 1 2 2v3a2 2 0 0 1-2 2A18 18 0 0 1 2 4a2 2 0 0 1 2-2h3a2 2 0 0 1 2 2v3a2 2 0 0 1-.8 1.6l-.468.351a1 1 0 0 0-.292 1.233 14 14 0 0 0 6.392 6.384'));
    ctx.restore();
  }

  /* ----- alarm ----- */
  private drawAlarmScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.55);
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      this.drawLucideAlarm(ctx, IR, C);
    } else {
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontEnglish(IR * 0.43);
      ctx.fillText('07:30', 0, -IR * 0.07);
      if (this.alarmCopy !== 'none') {
        ctx.fillStyle = rgba(C, 0.62);
        ctx.font = fontBody(IR * 0.105);
        ctx.fillText(this.alarmCopy === 'name' ? '早安，Nyabula' : '喝水 · 吃药', 0, IR * 0.26);
      }
    }
  }

  /* ----- call ----- */
  private drawCallScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.7);
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.globalAlpha *= sceneEase(this.optionFade);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      if (this.callState === 'ended') this.drawMaterialIcon(ctx, 'call_end', IR, C, { size: 1.10, alpha: 0.72 });
      else this.drawLucidePhoneCall(ctx, IR, C, this.callState === 'incoming');
    } else {
      ctx.fillStyle = rgba(C, this.callState === 'ended' ? 0.58 : 0.98);
      ctx.font = this.callState === 'ended' ? fontTitle(IR * 0.25) : fontEnglish(IR * 0.27);
      ctx.fillText(this.callState === 'ended' ? '通话结束' : 'BEACON', 0, -IR * 0.11);
      ctx.fillStyle = rgba(C, 0.48);
      ctx.font = fontMixed(IR * 0.10);
      const detail =
        this.callState === 'incoming' ? '+86 138 0013 8000'
        : this.callState === 'active' ? '00:42 · 通话中'
        : '通话时长 03:18';
      ctx.fillText(detail, 0, IR * 0.17);
    }
  }

  /* ----- task ----- */
  private drawTaskStateVisual(
    ctx: CanvasRenderingContext2D,
    IR: number,
    C: number[],
    state: string,
    alpha = 1,
    reveal = this.iconReveal(),
  ): void {
    const tNow = this.tNow;
    ctx.save();
    ctx.globalAlpha *= alpha * this.iconSceneAlpha();
    if (state === 'running') {
      const nodes = 6;
      const rotation = tNow * 0.72;
      ctx.lineCap = 'round';
      ctx.lineWidth = IR * 0.018;
      for (let i = 0; i < nodes; i++) {
        const a = rotation + (i * Math.PI * 2) / nodes;
        const b = rotation + ((i + 1) * Math.PI * 2) / nodes;
        ctx.strokeStyle = rgba(C, 0.16 + i * 0.025);
        ctx.beginPath();
        ctx.moveTo(Math.cos(a) * IR * 0.48, Math.sin(a) * IR * 0.48);
        ctx.lineTo(Math.cos(b) * IR * 0.48, Math.sin(b) * IR * 0.48);
        ctx.stroke();
      }
      const active = (tNow * 2.4) % nodes;
      for (let i = 0; i < nodes; i++) {
        const a = rotation + (i * Math.PI * 2) / nodes;
        const d = Math.min(Math.abs(i - active), nodes - Math.abs(i - active));
        ctx.fillStyle = rgba(C, clamp(0.32 + (1 - d) * 0.55, 0.28, 0.95));
        ctx.beginPath();
        ctx.arc(Math.cos(a) * IR * 0.48, Math.sin(a) * IR * 0.48, IR * (d < 0.7 ? 0.064 : 0.036), 0, 7);
        ctx.fill();
      }
      const breathe = 0.90 + 0.10 * Math.sin(tNow * 4.2);
      ctx.fillStyle = rgba(C, 0.94);
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.082 * breathe, 0, 7);
      ctx.fill();
    } else if (state === 'queued') {
      ctx.save();
      ctx.translate(0, -IR * 0.07);
      this.drawMaterialIcon(ctx, 'task_queued', IR, C, { size: 0.88, reveal, rotation: Math.sin(tNow * 2.1) * 0.055, sceneAlpha: 1 });
      ctx.restore();
      for (let i = 0; i < 3; i++) {
        const active = Math.floor(tNow * 2.2) % 3 === i;
        ctx.fillStyle = rgba(C, active ? 0.92 : 0.24);
        ctx.beginPath();
        ctx.arc((i - 1) * IR * 0.16, IR * 0.48, IR * (active ? 0.035 : 0.025), 0, 7);
        ctx.fill();
      }
    } else if (state === 'confirm') {
      const breathe = 0.96 + 0.04 * Math.sin(tNow * 2.8);
      const offset = IR * (0.50 + 0.025 * Math.sin(tNow * 2.8));
      ctx.save();
      ctx.scale(breathe, breathe);
      this.drawMaterialIcon(ctx, 'task_confirm', IR, C, { size: 1.02, reveal, sceneAlpha: 1 });
      ctx.restore();
      ctx.strokeStyle = rgba(C, 0.46);
      ctx.lineWidth = IR * 0.022;
      ctx.lineCap = 'round';
      for (const dir of [-1, 1]) {
        ctx.beginPath();
        ctx.moveTo(dir * offset, -IR * 0.18);
        ctx.lineTo(dir * offset, IR * 0.18);
        ctx.stroke();
      }
    } else if (state === 'done') {
      const elapsed = tNow - Math.max(this.sceneStartedAt, this.optionChangedAt);
      const p = clamp(elapsed / 0.78, 0, 1);
      this.drawMaterialIcon(ctx, 'task_done', IR, C, { size: 1.12, reveal, sceneAlpha: 1 });
      if (p < 1) {
        const e = sceneEase(p);
        ctx.strokeStyle = rgba(C, (1 - e) * 0.48);
        ctx.lineWidth = IR * 0.022;
        ctx.beginPath();
        ctx.arc(0, 0, IR * (0.30 + 0.35 * e), 0, 7);
        ctx.stroke();
      }
    } else {
      const phase = tNow % 2.4;
      const shake = phase < 0.34 ? Math.sin(phase * 58) * (1 - phase / 0.34) * IR * 0.020 : 0;
      ctx.save();
      ctx.translate(shake, 0);
      this.drawMaterialIcon(ctx, 'task_failed', IR, C, { size: 1.04, reveal, alpha: 0.78 + 0.14 * Math.sin(tNow * 2.6), sceneAlpha: 1 });
      ctx.restore();
      if (phase < 0.62) {
        const p = phase / 0.62;
        ctx.strokeStyle = rgba(C, (1 - p) * 0.28);
        ctx.lineWidth = IR * 0.018;
        ctx.beginPath();
        ctx.arc(0, 0, IR * (0.39 + 0.17 * sceneEase(p)), 0, 7);
        ctx.stroke();
      }
    }
    ctx.restore();
  }

  private drawTaskScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const metas: Record<string, [number, string, string]> = {
      running: [0.62, '3/5', '执行中'],
      queued: [0.14, '1/5', '排队中'],
      confirm: [0.46, '2/5', '等待确认'],
      done: [1, '5/5', '已完成'],
      failed: [0.68, '3/5', '执行失败'],
    };
    const meta = metas[this.taskState] ?? metas.running;
    const C = this.sceneGlow(ctx, IC, 0.55);
    const progress = meta[0];
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      if (this.taskPrevState) this.drawTaskStateVisual(ctx, IR, C, this.taskPrevState, 1 - sceneEase(this.optionFade), 1);
      this.drawTaskStateVisual(ctx, IR, C, this.taskState, sceneEase(this.optionFade));
    } else {
      ctx.globalAlpha *= sceneEase(this.optionFade);
      ctx.strokeStyle = rgba(C, 0.18);
      ctx.lineWidth = IR * 0.05;
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.55, -Math.PI / 2, Math.PI * 1.5);
      ctx.stroke();
      ctx.strokeStyle = rgba(C, 0.88);
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.55, -Math.PI / 2, -Math.PI / 2 + progress * Math.PI * 2);
      ctx.stroke();
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontEnglish(IR * 0.39);
      ctx.fillText(meta[1], 0, -IR * 0.04);
      ctx.fillStyle = rgba(C, 0.52);
      ctx.font = fontTitle(IR * 0.10);
      ctx.fillText(meta[2], 0, IR * 0.29);
    }
  }

  /* ----- stopwatch ----- */
  private drawStopwatchScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean, startedAt: number): void {
    const C = this.sceneGlow(ctx, IC, 0.58);
    const elapsed = this.tNow - startedAt;
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    const minutes = Math.floor(elapsed / 60);
    const seconds = Math.floor(elapsed % 60);
    const tenths = Math.floor((elapsed % 1) * 10);
    this.drawDial(ctx, IR, C, (elapsed % 60) / 60);
    ctx.fillStyle = rgba(C, 0.98);
    if (side < 0) {
      ctx.font = fontEnglish(IR * 0.58);
      ctx.fillText(String(minutes).padStart(2, '0'), 0, -IR * 0.04);
      ctx.font = fontBody(IR * 0.085);
      ctx.fillStyle = rgba(C, 0.44);
      ctx.fillText('分钟', 0, IR * 0.34);
    } else {
      ctx.font = fontEnglish(IR * 0.47);
      ctx.fillText(`${String(seconds).padStart(2, '0')}.${tenths}`, 0, -IR * 0.04);
      ctx.font = fontBody(IR * 0.085);
      ctx.fillStyle = rgba(C, 0.44);
      ctx.fillText('秒钟', 0, IR * 0.34);
    }
  }

  /* ----- calendar ----- */
  private drawCalendarScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.50);
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      ctx.strokeStyle = rgba(C, 0.24);
      ctx.lineWidth = IR * 0.025;
      ctx.beginPath();
      ctx.roundRect(-IR * 0.48, -IR * 0.48, IR * 0.96, IR * 0.96, IR * 0.10);
      ctx.stroke();
      ctx.fillStyle = rgba(C, 0.42);
      ctx.fillRect(-IR * 0.48, -IR * 0.29, IR * 0.96, IR * 0.025);
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontEnglish(IR * 0.50);
      ctx.fillText('16', 0, IR * 0.05);
      ctx.fillStyle = rgba(C, 0.52);
      ctx.font = fontTitle(IR * 0.11);
      ctx.fillText('8 月 · 星期日', 0, IR * 0.36);
    } else {
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontEnglish(IR * 0.40);
      ctx.fillText('09:30', 0, -IR * 0.18);
      ctx.fillStyle = rgba(C, 0.67);
      ctx.font = fontTitle(IR * 0.14);
      ctx.fillText('产品设计评审', 0, IR * 0.13);
      ctx.fillStyle = rgba(C, 0.34);
      ctx.font = fontBody(IR * 0.085);
      ctx.fillText('还有 25 分钟', 0, IR * 0.35);
    }
  }

  /* ----- sleep timer ----- */
  private drawSleepTimerScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean, startedAt: number): void {
    const C = this.sceneGlow(ctx, IC, 0.45);
    this.drawSceneBackdrop(ctx, IR, minimal, 0.74);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      ctx.save();
      ctx.translate(-IR * 0.02, IR * 0.01);
      ctx.shadowColor = rgba(C, 0.42);
      ctx.shadowBlur = IR * 0.08;
      this.drawMaterialIcon(ctx, 'moon', IR, C, { size: 1.04, reveal: this.iconReveal(startedAt) });
      ctx.restore();
      const stars: Array<[number, number, number]> = [[-0.46, -0.34, 0.13], [0.40, -0.35, 0.16], [0.47, 0.25, 0.10]];
      for (let i = 0; i < stars.length; i++) {
        const [x, y, s] = stars[i];
        const pulse = 0.28 + 0.42 * (0.5 + 0.5 * Math.sin(this.tNow * 1.35 + i * 1.9));
        ctx.save();
        ctx.translate(IR * x, IR * y);
        ctx.rotate(Math.sin(this.tNow * 0.25 + i) * 0.10);
        this.drawMaterialIcon(ctx, 'star', IR, C, { size: s * 1.75, reveal: this.iconReveal(startedAt), alpha: pulse });
        ctx.restore();
      }
    } else {
      const remaining = Math.max(0, 1800 - (this.tNow - startedAt));
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontEnglish(IR * 0.48);
      ctx.fillText(String(Math.ceil(remaining / 60)), 0, -IR * 0.12);
      ctx.fillStyle = rgba(C, 0.55);
      ctx.font = fontTitle(IR * 0.105);
      ctx.fillText('分钟', 0, IR * 0.18);
      ctx.fillStyle = rgba(C, 0.30);
      ctx.font = fontBody(IR * 0.08);
      ctx.fillText('音乐结束后休眠', 0, IR * 0.36);
    }
  }

  /* ----- network ----- */
  private drawNetworkScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.55);
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    const iconMap: Record<string, string> = { wifi: 'wifi', bluetooth: 'bluetooth', offline: 'wifi_off' };
    if (side < 0) {
      const icon = iconMap[this.networkState] ?? 'wifi';
      if (this.networkPrevState) {
        const previous = iconMap[this.networkPrevState] ?? 'wifi';
        this.drawMaterialIcon(ctx, previous, IR, C, { size: 1.18, reveal: 1, alpha: 1 - sceneEase(this.optionFade) });
      }
      this.drawMaterialIcon(ctx, icon, IR, C, { size: 1.18, alpha: (this.networkState === 'offline' ? 0.72 : 1) * sceneEase(this.optionFade) });
    } else {
      ctx.globalAlpha *= sceneEase(this.optionFade);
      const title = this.networkState === 'wifi' ? '网络已连接' : this.networkState === 'bluetooth' ? '手机已连接' : '当前离线';
      const detail = this.networkState === 'wifi' ? 'Nyabula · 5 GHz' : this.networkState === 'bluetooth' ? 'BeaconPhone' : '等待重新连接';
      ctx.fillStyle = rgba(C, this.networkState === 'offline' ? 0.54 : 0.98);
      ctx.font = fontTitle(IR * 0.22);
      ctx.fillText(title, 0, -IR * 0.13);
      ctx.fillStyle = rgba(C, 0.44);
      ctx.font = fontMixed(IR * 0.09);
      ctx.fillText(detail, 0, IR * 0.16);
      if (this.networkState !== 'offline') {
        const pulse = 0.55 + 0.35 * Math.sin(this.tNow * 2.2);
        ctx.fillStyle = rgba(C, pulse);
        ctx.beginPath();
        ctx.arc(0, IR * 0.38, IR * 0.025, 0, 7);
        ctx.fill();
      }
    }
  }

  /* ----- audio route ----- */
  private drawAudioScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.55);
    const muted = this.audioRoute === 'mute';
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    const iconMap: Record<string, string> = { speaker: 'speaker', headphone: 'headphones', both: 'audio_both', mute: 'audio_mute' };
    if (side < 0) {
      this.drawDial(ctx, IR, C, muted ? 0 : 0.64);
      const icon = iconMap[this.audioRoute] ?? 'speaker';
      if (this.audioPrevRoute) {
        const previous = iconMap[this.audioPrevRoute] ?? 'speaker';
        this.drawMaterialIcon(ctx, previous, IR, C, { size: 0.82, reveal: 1, alpha: 1 - sceneEase(this.optionFade) });
      }
      this.drawMaterialIcon(ctx, icon, IR, C, { size: 0.82, alpha: (muted ? 0.64 : 1) * sceneEase(this.optionFade) });
    } else {
      ctx.globalAlpha *= sceneEase(this.optionFade);
      const labels: Record<string, string> = { speaker: '扬声器', headphone: '耳机', both: '同时输出', mute: '已静音' };
      const label = labels[this.audioRoute] ?? labels.speaker;
      ctx.fillStyle = rgba(C, muted ? 0.48 : 0.98);
      ctx.font = fontTitle(IR * 0.22);
      ctx.fillText(label, 0, -IR * 0.14);
      ctx.fillStyle = rgba(C, 0.74);
      ctx.font = fontEnglish(IR * 0.36);
      ctx.fillText(muted ? '—' : '64%', 0, IR * 0.14);
      ctx.fillStyle = rgba(C, 0.34);
      ctx.font = fontBody(IR * 0.085);
      ctx.fillText('媒体音量', 0, IR * 0.39);
    }
  }

  /* ----- EQ ----- */
  private drawEqCurve(ctx: CanvasRenderingContext2D, IR: number, C: number[], calibrating = false): void {
    ctx.strokeStyle = rgba(C, 0.14);
    ctx.lineWidth = IR * 0.010;
    for (let i = -2; i <= 2; i++) {
      ctx.beginPath();
      ctx.moveTo(-IR * 0.62, i * IR * 0.16);
      ctx.lineTo(IR * 0.62, i * IR * 0.16);
      ctx.stroke();
    }
    const sweep = ((this.tNow * 0.20) % 1) * IR * 1.24 - IR * 0.62;
    ctx.strokeStyle = rgba(C, 0.88);
    ctx.lineWidth = IR * 0.025;
    ctx.lineCap = 'round';
    ctx.beginPath();
    for (let i = 0; i <= 48; i++) {
      const x = IR * (-0.62 + (i / 48) * 1.24);
      const nx = x / IR;
      const y = IR * (-0.04 - Math.exp(-Math.pow((nx + 0.28) * 4, 2)) * 0.16 + Math.exp(-Math.pow((nx - 0.22) * 5, 2)) * 0.11);
      if (i) ctx.lineTo(x, y);
      else ctx.moveTo(x, y);
    }
    ctx.stroke();
    if (calibrating) {
      const g = ctx.createLinearGradient(sweep - IR * 0.15, 0, sweep, 0);
      g.addColorStop(0, rgba(C, 0));
      g.addColorStop(1, rgba(C, 0.34));
      ctx.fillStyle = g;
      ctx.fillRect(sweep - IR * 0.15, -IR * 0.42, IR * 0.15, IR * 0.84);
    }
  }

  private drawEqScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean, startedAt: number): void {
    const C = this.sceneGlow(ctx, IC, 0.55);
    const cal = this.eqView === 'calibrate';
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.globalAlpha *= sceneEase(this.optionFade);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      if (cal) {
        const p = ((this.tNow - startedAt) * 0.13) % 1;
        this.drawDial(ctx, IR, C, p);
        ctx.fillStyle = rgba(C, 0.98);
        ctx.font = fontEnglish(IR * 0.33);
        ctx.fillText(`${Math.round(20 * Math.pow(1000, p))}`, 0, -IR * 0.04);
        ctx.fillStyle = rgba(C, 0.42);
        ctx.font = fontMixed(IR * 0.085);
        ctx.fillText('Hz · 扫频', 0, IR * 0.28);
      } else {
        const bars = 7;
        for (let i = 0; i < bars; i++) {
          const x = (i - (bars - 1) / 2) * IR * 0.16;
          const h = IR * (0.22 + 0.28 * (0.5 + 0.5 * Math.sin(i * 1.6 + this.tNow * 0.7)));
          ctx.fillStyle = rgba(C, 0.30 + i * 0.07);
          ctx.beginPath();
          ctx.roundRect(x - IR * 0.035, -h / 2, IR * 0.07, h, IR * 0.035);
          ctx.fill();
        }
      }
    } else {
      ctx.save();
      ctx.translate(0, -IR * 0.08);
      this.drawEqCurve(ctx, IR, C, cal);
      ctx.restore();
      ctx.fillStyle = rgba(C, 0.58);
      ctx.font = cal ? fontTitle(IR * 0.10) : fontEnglish(IR * 0.085);
      ctx.fillText(cal ? '测量中 · 2/4' : 'NYABULA FLAT', 0, IR * 0.43);
    }
  }

  /* ----- caption ----- */
  private drawCaptionScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.48);
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      ctx.strokeStyle = rgba(C, 0.82);
      ctx.lineWidth = IR * 0.025;
      ctx.lineCap = 'round';
      ctx.beginPath();
      for (let i = 0; i <= 60; i++) {
        const x = IR * (-0.58 + (i / 60) * 1.16);
        const env = Math.sin((i / 60) * Math.PI);
        const y = Math.sin(i * 0.72 + this.tNow * 5) * env * IR * 0.25;
        if (i) ctx.lineTo(x, y);
        else ctx.moveTo(x, y);
      }
      ctx.stroke();
      ctx.fillStyle = rgba(C, 0.40);
      ctx.font = fontTitle(IR * 0.09);
      ctx.fillText('聆听中', 0, IR * 0.42);
    } else {
      ctx.fillStyle = rgba(C, 0.28);
      ctx.font = fontBody(IR * 0.095);
      ctx.fillText(typeof this.scenePayload.previous_line === 'string' ? this.scenePayload.previous_line : '我可以帮你', 0, -IR * 0.27);
      ctx.fillStyle = rgba(C, 0.96);
      ctx.font = fontTitle(IR * 0.15);
      ctx.fillText(typeof this.scenePayload.current_line === 'string' ? this.scenePayload.current_line : typeof this.scenePayload.text === 'string' ? this.scenePayload.text : '设置一个提醒', 0, IR * 0.01, IR * 1.65);
      ctx.fillStyle = rgba(C, 0.34);
      ctx.font = fontBody(IR * 0.085);
      ctx.fillText(typeof this.scenePayload.next_line === 'string' ? this.scenePayload.next_line : '字幕模式已开启', 0, IR * 0.31);
    }
  }

  /* ----- briefing ----- */
  private drawBriefingScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.48);
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      const p = (this.tNow * 0.16) % 1;
      ctx.strokeStyle = rgba(C, 0.18);
      ctx.lineWidth = IR * 0.018;
      ctx.beginPath();
      ctx.arc(0, 0, IR * 0.48, 0, 7);
      ctx.stroke();
      for (let i = 0; i < 3; i++) {
        const a = -Math.PI / 2 + ((i + p) * Math.PI * 2) / 3;
        const r = IR * 0.48;
        ctx.fillStyle = rgba(C, i === 1 ? 0.92 : 0.40);
        ctx.beginPath();
        ctx.arc(Math.cos(a) * r, Math.sin(a) * r, IR * (i === 1 ? 0.065 : 0.035), 0, 7);
        ctx.fill();
      }
      ctx.strokeStyle = rgba(C, 0.78);
      ctx.lineWidth = IR * 0.026;
      ctx.lineCap = 'round';
      ctx.beginPath();
      for (let i = 0; i <= 40; i++) {
        const x = IR * (-0.31 + (i / 40) * 0.62);
        const y = Math.sin(i * 0.66 + this.tNow * 4) * IR * 0.09 * (1 - Math.abs(i / 20 - 1));
        if (i) ctx.lineTo(x, y);
        else ctx.moveTo(x, y);
      }
      ctx.stroke();
    } else {
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontTitle(IR * 0.22);
      ctx.fillText('早间简报', 0, -IR * 0.24);
      ctx.fillStyle = rgba(C, 0.55);
      ctx.font = fontBody(IR * 0.10);
      ctx.fillText('天气 · 日程 · 资讯', 0, IR * 0.03);
      ctx.fillStyle = rgba(C, 0.36);
      ctx.font = fontMixed(IR * 0.085);
      ctx.fillText('正在朗读  1 / 3', 0, IR * 0.29);
    }
  }

  /* ----- privacy ----- */
  private drawPrivacyScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.60);
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      this.drawMaterialIcon(ctx, 'privacy', IR, C, { size: 1.16, alpha: 0.88 + 0.12 * Math.sin(this.tNow * 2.4) });
    } else {
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontTitle(IR * 0.17);
      ctx.fillText('摄像头 + 麦克风', 0, -IR * 0.20);
      ctx.fillStyle = rgba(C, 0.72);
      ctx.font = fontTitle(IR * 0.14);
      ctx.fillText('正在使用', 0, IR * 0.06);
      ctx.fillStyle = rgba(C, 0.34);
      ctx.font = fontBody(IR * 0.08);
      ctx.fillText('本地处理 · 指示不可关闭', 0, IR * 0.31);
    }
  }

  /* ----- identity ----- */
  private drawIdentityScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.55);
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      this.drawMaterialIcon(ctx, 'identity', IR, C, { size: 1.18 });
    } else {
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontTitle(IR * 0.23);
      ctx.fillText('你好，主人', 0, -IR * 0.15);
      ctx.fillStyle = rgba(C, 0.58);
      ctx.font = fontTitle(IR * 0.105);
      ctx.fillText('主人身份已确认', 0, IR * 0.14);
      ctx.fillStyle = rgba(C, 0.30);
      ctx.font = fontBody(IR * 0.075);
      ctx.fillText('模板仅保存在本机', 0, IR * 0.34);
    }
  }

  /* ----- memory ----- */
  private drawMemoryScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.48);
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      this.drawMaterialIcon(ctx, 'memory', IR, C, { size: 1.16, alpha: 0.90 + 0.10 * Math.sin(this.tNow * 1.7) });
    } else {
      ctx.fillStyle = rgba(C, 0.55);
      ctx.font = fontBody(IR * 0.09);
      ctx.fillText('准备记住', 0, -IR * 0.31);
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontTitle(IR * 0.17);
      ctx.fillText('你喜欢轻音乐', 0, -IR * 0.04);
      ctx.fillStyle = rgba(C, 0.42);
      ctx.font = fontBody(IR * 0.085);
      ctx.fillText('等待语音确认', 0, IR * 0.26);
    }
  }

  /* ----- devices ----- */
  private drawDevicesScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.54);
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      this.drawMaterialIcon(ctx, 'devices', IR, C, { size: 1.18 });
    } else {
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontEnglish(IR * 0.37);
      ctx.fillText('2', 0, -IR * 0.18);
      ctx.fillStyle = rgba(C, 0.58);
      ctx.font = fontTitle(IR * 0.11);
      ctx.fillText('台设备在线', 0, IR * 0.08);
      ctx.fillStyle = rgba(C, 0.32);
      ctx.font = fontMixed(IR * 0.075);
      ctx.fillText('手机 · CODEX · 局域网 MCP', 0, IR * 0.31);
    }
  }

  /* ----- system ----- */
  private drawSystemScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.48);
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      this.drawMaterialIcon(ctx, 'system', IR, C, { size: 1.18 });
    } else {
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontEnglish(IR * 0.22);
      ctx.fillText('OPENVELA', 0, -IR * 0.24);
      ctx.fillStyle = rgba(C, 0.58);
      ctx.font = fontTitle(IR * 0.105);
      ctx.fillText('本地大脑就绪', 0, IR * 0.01);
      ctx.fillStyle = rgba(C, 0.31);
      ctx.font = fontMixed(IR * 0.08);
      ctx.fillText('AMP · NPU 可用', 0, IR * 0.27);
    }
  }

  /* ----- health ----- */
  private drawHealthScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.58);
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      this.drawMaterialIcon(ctx, 'heart_rate', IR, C, { size: 1.18, alpha: 0.90 + 0.10 * Math.sin(this.tNow * 4.0) });
    } else {
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontEnglish(IR * 0.48);
      ctx.fillText('72', 0, -IR * 0.13);
      ctx.fillStyle = rgba(C, 0.56);
      ctx.font = fontBody(IR * 0.10);
      ctx.fillText('次/分 · 信号良好', 0, IR * 0.16);
      ctx.fillStyle = rgba(C, 0.28);
      ctx.font = fontBody(IR * 0.07);
      ctx.fillText('趋势参考 · 非医疗用途', 0, IR * 0.37);
    }
  }

  /* ----- presence ----- */
  private drawPresenceScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.50);
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      this.drawMaterialIcon(ctx, 'presence', IR, C, { size: 1.18 });
    } else {
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontTitle(IR * 0.22);
      ctx.fillText('用户在场', 0, -IR * 0.20);
      ctx.fillStyle = rgba(C, 0.68);
      ctx.font = fontMixed(IR * 0.31);
      ctx.fillText('0.8 m', 0, IR * 0.09);
      ctx.fillStyle = rgba(C, 0.30);
      ctx.font = fontBody(IR * 0.075);
      ctx.fillText('正在看向猫猫', 0, IR * 0.34);
    }
  }

  /* ----- companion ----- */
  private drawCompanionScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.43);
    this.drawSceneBackdrop(ctx, IR, minimal, 0.74);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      /* Pass one slow breath from the centre outwards, then leave a long rest.
       * Staggering the layers feels organic without becoming a ripple loop. */
      const cycle = 6.8;
      const phase = this.tNow % cycle;
      const layerBreath = (delay: number): number => {
        const p = phase - delay;
        if (p < 0 || p >= 2.15) return 0;
        if (p < 1.05) return sceneEase(p / 1.05);
        if (p < 1.30) return 1;
        return 1 - sceneEase((p - 1.30) / 0.85);
      };
      for (let i = 3; i >= 0; i--) {
        const breath = layerBreath(i * 0.22);
        const radius = IR * (0.22 + i * 0.11) * (0.90 + breath * 0.24);
        ctx.fillStyle = rgba(C, 0.050 + i * 0.032 + breath * 0.026);
        ctx.beginPath();
        ctx.arc(0, 0, radius, 0, 7);
        ctx.fill();
      }
      this.drawMaterialIcon(ctx, 'companion', IR, C, { size: 0.48, alpha: 0.94 });
    } else {
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontTitle(IR * 0.22);
      ctx.fillText('专注陪伴', 0, -IR * 0.17);
      ctx.fillStyle = rgba(C, 0.50);
      ctx.font = fontBody(IR * 0.09);
      ctx.fillText('安静模式', 0, IR * 0.10);
      ctx.fillStyle = rgba(C, 0.27);
      ctx.font = fontBody(IR * 0.075);
      ctx.fillText('仅保留重要提醒', 0, IR * 0.32);
    }
  }

  /* ----- cat home ----- */
  private drawCatHomeScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.46);
    this.drawSceneBackdrop(ctx, IR, minimal, 0.72);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      ctx.strokeStyle = rgba(C, 0.78);
      ctx.lineWidth = IR * 0.026;
      ctx.lineCap = 'round';
      ctx.lineJoin = 'round';
      ctx.beginPath();
      ctx.moveTo(-IR * 0.48, IR * 0.12);
      ctx.lineTo(0, -IR * 0.37);
      ctx.lineTo(IR * 0.48, IR * 0.12);
      ctx.lineTo(IR * 0.37, IR * 0.12);
      ctx.lineTo(IR * 0.37, IR * 0.42);
      ctx.lineTo(-IR * 0.37, IR * 0.42);
      ctx.lineTo(-IR * 0.37, IR * 0.12);
      ctx.closePath();
      ctx.stroke();
      ctx.beginPath();
      ctx.arc(0, IR * 0.20, IR * 0.14, Math.PI, 0);
      ctx.lineTo(IR * 0.14, IR * 0.42);
      ctx.lineTo(-IR * 0.14, IR * 0.42);
      ctx.closePath();
      ctx.stroke();
      ctx.fillStyle = rgba(C, 0.38 + 0.22 * Math.sin(this.tNow * 0.8));
      ctx.beginPath();
      ctx.arc(IR * 0.34, -IR * 0.30, IR * 0.055, 0, 7);
      ctx.fill();
    } else {
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontTitle(IR * 0.22);
      ctx.fillText('虚拟猫舍', 0, -IR * 0.23);
      ctx.fillStyle = rgba(C, 0.58);
      ctx.font = fontBody(IR * 0.11);
      ctx.fillText('晴 · 午后', 0, IR * 0.03);
      ctx.fillStyle = rgba(C, 0.30);
      ctx.font = fontBody(IR * 0.08);
      ctx.fillText('双屏立体视窗预览', 0, IR * 0.29);
    }
  }

  /* ----- subwoofer ----- */
  private drawSubwooferScene(ctx: CanvasRenderingContext2D, IR: number, IC: RGB, side: number, minimal: boolean): void {
    const C = this.sceneGlow(ctx, IC, 0.58);
    this.drawSceneBackdrop(ctx, IR, minimal);
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (side < 0) {
      const beat = 0.5 + 0.5 * Math.sin(this.tNow * 4.4);
      this.drawMaterialIcon(ctx, 'subwoofer', IR, C, { size: 1.18 + beat * 0.025, alpha: 0.84 + 0.16 * beat });
    } else {
      ctx.fillStyle = rgba(C, 0.98);
      ctx.font = fontMixed(IR * 0.27);
      ctx.fillText('2.1 已启用', 0, -IR * 0.22);
      ctx.fillStyle = rgba(C, 0.70);
      ctx.font = fontMixed(IR * 0.33);
      ctx.fillText('80 Hz', 0, IR * 0.08);
      ctx.fillStyle = rgba(C, 0.31);
      ctx.font = fontMixed(IR * 0.075);
      ctx.fillText('低音在线 · LR 分频', 0, IR * 0.34);
    }
  }
}
