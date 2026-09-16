/* Native Core uses monotonic milliseconds, never the browser wall clock.
 * Convert a copied snapshot at receipt; leave rendering independent of Core. */
import type { EyeState } from './types.js';

export interface CoreEyeSnapshot {
  schema: 'nyabula.eye.v1';
  seq: number;
  uptime_ms: number;
  expression: string;
  expression_since_ms: number;
  scene: string;
  scene_style: 'full' | 'minimal';
  scene_since_ms: number;
  scene_payload: Record<string, unknown>;
  gaze_x: number;
  gaze_y: number;
  gaze_until_ms: number;
  gaze_active: boolean;
  ambient_light: number;
  auto_blink: boolean;
  iris_rgb: [number, number];
  blink_nonce: number;
  blink_eyes: number;
}

export function coreEyeState(s: CoreEyeSnapshot, receivedAt = Date.now()): EyeState {
  const epoch = (t: number) => receivedAt - Math.max(0, s.uptime_ms - t);
  const p = s.scene_payload;
  const hex = (n: number) => `#${(n & 0xffffff).toString(16).padStart(6, '0')}`;
  return {
    seq: s.seq, t: receivedAt, autoBlink: s.auto_blink,
    expression: { mode: s.expression, since: epoch(s.expression_since_ms), lightLevel: s.ambient_light },
    gaze: { mode: s.gaze_active ? 'target' : 'auto', x: s.gaze_x, y: s.gaze_y,
      holdUntil: receivedAt + Math.max(0, s.gaze_until_ms - s.uptime_ms) },
    appearance: { irisLeft: hex(s.iris_rgb[0]), irisRight: hex(s.iris_rgb[1]) },
    scene: { type: s.scene === 'none' ? null : s.scene.replaceAll('_', '-'),
      style: s.scene_style, since: epoch(s.scene_since_ms), payload: { ...p },
      options: { weather: p.weather, musicView: p.music_view, battery: p.battery_state,
        alarmCopy: p.alarm_copy, call: p.call_state, task: p.task_state,
        network: p.network_state, audio: p.audio_route === 'headphones' ? 'headphone' : p.audio_route,
        eq: p.eq_view === 'calibrating' ? 'calibrate' : p.eq_view } },
    blinkNonce: s.blink_nonce,
    blinkEyes: s.blink_eyes,
  };
}

export function isCoreEyeSnapshot(value: unknown): value is CoreEyeSnapshot {
  if (!value || typeof value !== 'object') return false;
  const s = value as Record<string, unknown>;
  const numbers = ['seq','uptime_ms','expression_since_ms','scene_since_ms','gaze_x','gaze_y',
    'gaze_until_ms','ambient_light','blink_nonce','blink_eyes'];
  return s.schema === 'nyabula.eye.v1' && numbers.every(k => typeof s[k] === 'number' && Number.isFinite(s[k]))
    && typeof s.expression === 'string' && typeof s.scene === 'string'
    && (s.scene_style === 'full' || s.scene_style === 'minimal')
    && typeof s.gaze_active === 'boolean' && typeof s.auto_blink === 'boolean'
    && !!s.scene_payload && typeof s.scene_payload === 'object' && !Array.isArray(s.scene_payload)
    && Array.isArray(s.iris_rgb) && s.iris_rgb.length === 2
    && s.iris_rgb.every(n => Number.isInteger(n) && n >= 0 && n <= 0xffffff);
}
