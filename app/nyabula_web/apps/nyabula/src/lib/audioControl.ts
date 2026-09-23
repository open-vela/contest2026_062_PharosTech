/* Pure helpers for the `audio.*` topics of the ES8388 codec service
 * (app/nyabula_core/ny_product_audio.c): wire types, a parser that refuses a
 * malformed status instead of rendering half of one, and the labels the
 * feature page and its card share.
 *
 * No Vue / DOM imports: this file is unit-tested under plain `node --test`. */

export type AudioOutputRoute = 'auto' | 'headphones' | 'speaker' | 'both' | 'off';
export type AudioInputRoute = 'main' | 'headset' | 'both' | 'off';

export interface AudioOutputStatus {
  /** What the owner asked for. */
  route: AudioOutputRoute;
  /** What `auto` resolved to; equals `route` for a fixed route. */
  activeRoute: AudioOutputRoute;
  headphones: boolean;
  volume: number;
  muted: boolean;
  mono: boolean;
  swap: boolean;
  invertLeft: boolean;
  invertRight: boolean;
}
export interface AudioInputStatus {
  route: AudioInputRoute;
  muted: boolean;
  gain: number;
  /** What the codec PGA was given for `gain`; absent on a firmware that does not say. */
  gainDb: number;
}
export interface AudioStatus {
  /** The playback node answered; routes below are live, not remembered. */
  available: boolean;
  inputAvailable: boolean;
  settingsSaved: boolean;
  revision: number;
  output: AudioOutputStatus;
  input: AudioInputStatus;
}

export const OUTPUT_ROUTES: readonly { id: AudioOutputRoute; label: string }[] = [
  { id: 'auto', label: '自动' }, { id: 'headphones', label: '耳机' }, { id: 'speaker', label: '扬声器' },
  { id: 'both', label: '同时' }, { id: 'off', label: '关闭' },
];
export const INPUT_ROUTES: readonly { id: AudioInputRoute; label: string }[] = [
  { id: 'main', label: '主麦克风' }, { id: 'headset', label: '耳机麦克风' }, { id: 'both', label: '同时' }, { id: 'off', label: '关闭' },
];

const isRecord = (v: unknown): v is Record<string, unknown> => typeof v === 'object' && v !== null && !Array.isArray(v);
const percent = (v: unknown): number | null => (typeof v === 'number' && Number.isInteger(v) && v >= 0 && v <= 100 ? v : null);
const oneOf = <T extends string>(v: unknown, list: readonly { id: T }[]): T | null => list.find(item => item.id === v)?.id ?? null;

/** The firmware's mapping, mirrored for a status that lacks `gainDb`: nine 3 dB steps, nearest one. */
export function micGainDb(gain: number): number {
  const clamped = Math.min(100, Math.max(0, Math.round(gain)));
  return Math.floor((clamped * 8 + 50) / 100) * 3;
}

/** A status the page can render, or null when any field is missing or of the wrong kind. */
export function parseAudioStatus(raw: unknown): AudioStatus | null {
  if (!isRecord(raw) || !isRecord(raw.output) || !isRecord(raw.input)) return null;
  const o = raw.output;
  const i = raw.input;
  const route = oneOf(o.route, OUTPUT_ROUTES);
  const activeRoute = oneOf(o.activeRoute, OUTPUT_ROUTES);
  const inputRoute = oneOf(i.route, INPUT_ROUTES);
  const volume = percent(o.volume);
  const gain = percent(i.gain);
  const flags = [raw.available, o.headphones, o.muted, o.mono, o.swap, o.invertLeft, o.invertRight, i.muted];
  if (!route || !activeRoute || !inputRoute || volume === null || gain === null || flags.some(f => typeof f !== 'boolean')) return null;
  return {
    available: raw.available === true,
    inputAvailable: raw.inputAvailable !== false,
    settingsSaved: raw.settingsSaved !== false,
    revision: typeof raw.revision === 'number' && Number.isFinite(raw.revision) ? raw.revision : 0,
    output: {
      route, activeRoute, headphones: o.headphones === true, volume, muted: o.muted === true, mono: o.mono === true,
      swap: o.swap === true, invertLeft: o.invertLeft === true, invertRight: o.invertRight === true,
    },
    input: { route: inputRoute, muted: i.muted === true, gain, gainDb: typeof i.gainDb === 'number' && Number.isFinite(i.gainDb) ? i.gainDb : micGainDb(gain) },
  };
}

export const outputRouteLabel = (route: AudioOutputRoute): string => OUTPUT_ROUTES.find(r => r.id === route)?.label ?? route;
export const inputRouteLabel = (route: AudioInputRoute): string => INPUT_ROUTES.find(r => r.id === route)?.label ?? route;

/** Where sound comes out right now: `auto` names what it resolved to. */
export function activeOutputLabel(output: Pick<AudioOutputStatus, 'route' | 'activeRoute'>): string {
  if (output.activeRoute === 'off') return '输出已关闭';
  const active = output.activeRoute === 'both' ? '耳机+扬声器' : outputRouteLabel(output.activeRoute);
  return output.route === 'auto' ? `自动 · ${active}` : active;
}

/** One line for the card: level, route, jack. */
export function audioSummary(status: AudioStatus): string {
  if (!status.available) return '音频编解码器未就绪';
  const level = status.output.muted ? '静音' : `${status.output.volume}%`;
  return `${level} · ${activeOutputLabel(status.output)}${status.output.headphones ? ' · 耳机已插入' : ''}`;
}

/** The `audio_route` enum of the eye scene: it has no `off`, and a closed output is silent like a mute. */
export function sceneAudioRoute(output: Pick<AudioOutputStatus, 'activeRoute' | 'muted'>): 'speaker' | 'headphones' | 'both' | 'mute' {
  if (output.muted || output.activeRoute === 'off') return 'mute';
  // The driver always resolves `auto`; should it ever leak through, the board's fallback is the speaker.
  return output.activeRoute === 'auto' ? 'speaker' : output.activeRoute;
}

/** The device does not know the topic: a firmware without the audio service. */
export function isAudioUnsupported(e: unknown): boolean {
  return isRecord(e) && e.code === 'ENOTFOUND';
}
