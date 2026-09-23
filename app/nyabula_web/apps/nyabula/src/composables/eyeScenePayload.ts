/* Pure builders for the payloads of `eyes.scene.show` / `eyes.scene.update`.
 *
 * The device parser (app/nyabula/src/nyabula_eye_json.c) silently drops keys
 * it does not know, rejects the whole command when an integer field carries
 * a fraction or leaves its range, and replaces the WHOLE payload on every
 * update. So every builder returns a complete payload made only of accepted
 * keys, and sanitizeScenePayload() is the last line of defence in the store.
 *
 * No Vue / DOM imports: this file is unit-tested under plain `node --test`. */

export type ScenePayload = Record<string, unknown>;

/** Scene names the device accepts (wire spelling, '_' not '-'). */
export const CORE_SCENES = [
  'music', 'timer', 'weather', 'battery', 'alarm', 'call', 'task', 'stopwatch', 'calendar', 'sleep_timer',
  'network', 'audio', 'eq', 'caption', 'briefing', 'privacy', 'identity', 'memory', 'devices', 'system',
  'health', 'presence', 'companion', 'home', 'subwoofer', 'qr',
] as const;

/** Panel feature type -> wire scene name, or null when there is no such scene. */
export function coreSceneName(type: string): string | null {
  const name = type.replaceAll('-', '_');
  return (CORE_SCENES as readonly string[]).includes(name) ? name : null;
}

const ENUMS: Record<string, readonly string[]> = {
  weather: ['sunny', 'cloudy', 'rain', 'storm', 'snow', 'fog'],
  music_view: ['spectrum', 'lyrics'],
  battery_state: ['charging', 'low', 'full', 'hot', 'dock'],
  alarm_copy: ['name', 'reminder', 'none'],
  call_state: ['incoming', 'active', 'ended'],
  task_state: ['running', 'queued', 'confirm', 'done', 'failed'],
  network_state: ['wifi', 'bluetooth', 'offline'],
  audio_route: ['speaker', 'headphones', 'both', 'mute'],
  eq_view: ['profile', 'calibrating'],
};
const U8 = 255, U16 = 65535, U32 = 4294967295;
/** Integer fields: a fraction or an out-of-range value is -ERANGE on the device. */
const UINTS: Record<string, number> = {
  duration_ms: U32, position_ms: U32, remaining_ms: U32, elapsed_ms: U32, year: U16, month: U8, day: U8,
  hour: U8, minute: U8, percent: U8, device_count: U8, briefing_index: U8, briefing_count: U8,
};
const FLOATS = ['temperature_c', 'feels_like_c', 'humidity_percent', 'wind_kph', 'visibility_km', 'distance_m',
  'heart_rate_bpm', 'crossover_hz', 'progress'];
const BOOLS = ['active', 'playing', 'privacy_camera', 'privacy_microphone', 'signal_good'];
/** Text fields and their device buffer size minus the terminator (bytes of UTF-8). */
const TEXTS: Record<string, number> = {
  title: 47, subtitle: 47, detail: 47, value: 23, previous_line: 47, current_line: 47, next_line: 47,
};
export const SCENE_TEXT_BYTES = 47;
const EQ_BANDS = 10;

const encoder = new TextEncoder();
const bytes = (text: string): number => encoder.encode(text).length;

/** Longest prefix of whole code points that fits `max` bytes of UTF-8. */
function prefixWithin(text: string, max: number): string {
  let out = '';
  let used = 0;
  for (const ch of text) {
    const n = bytes(ch);
    if (used + n > max) break;
    out += ch;
    used += n;
  }
  return out;
}

/** Fit a caption into `max` bytes, marking a cut with an ellipsis. */
export function clipText(text: string, max = SCENE_TEXT_BYTES): string {
  const clean = text.replace(/\s+/g, ' ').trim();
  if (bytes(clean) <= max) return clean;
  return prefixWithin(clean, max - 3).trimEnd() + '…';
}

/** Break a text into at most `lines` chunks of `max` bytes; the last one is clipped. */
export function wrapText(text: string, lines: number, max = SCENE_TEXT_BYTES): string[] {
  let rest = text.replace(/\s+/g, ' ').trim();
  const out: string[] = [];
  while (rest && out.length < lines) {
    if (out.length === lines - 1) { out.push(clipText(rest, max)); break; }
    const head = prefixWithin(rest, max);
    out.push(head.trim());
    rest = rest.slice(head.length).trim();
  }
  return out;
}

/** Keep only what the device accepts, coerced into the accepted shape. */
export function sanitizeScenePayload(payload: ScenePayload | null | undefined): ScenePayload {
  const out: ScenePayload = {};
  for (const [key, value] of Object.entries(payload ?? {})) {
    if (value === undefined || value === null) continue;
    if (key in ENUMS) {
      if (typeof value === 'string' && ENUMS[key]!.includes(value)) out[key] = value;
    } else if (key in UINTS) {
      if (typeof value === 'number' && Number.isFinite(value)) out[key] = Math.min(UINTS[key]!, Math.max(0, Math.round(value)));
    } else if (FLOATS.includes(key)) {
      if (typeof value === 'number' && Number.isFinite(value)) out[key] = value;
    } else if (BOOLS.includes(key)) {
      if (typeof value === 'boolean') out[key] = value;
    } else if (key in TEXTS) {
      if (typeof value === 'string') out[key] = clipText(value, TEXTS[key]);
    } else if (key === 'eq_bands') {
      if (Array.isArray(value)) out[key] = Array.from({ length: EQ_BANDS }, (_, i) => (Number.isFinite(value[i]) ? Number(value[i]) : 0));
    }
  }
  return out;
}

/* ---- time ------------------------------------------------------------ */

export interface TimerSceneInput { durationMs: number; remainingMs: number; running: boolean; finished: boolean; label: string }
/** The device free-runs a demo countdown when remaining_ms is 0, so a live
 *  value never goes below 1; "finished" is a 1 ms timer that has run out,
 *  which the renderer draws as 00:00 with an empty ring. */
export function timerScene(t: TimerSceneInput): ScenePayload {
  const label = t.label.trim() || '倒计时';
  if (t.finished) return { duration_ms: 1, remaining_ms: 0, active: false, title: label, detail: '时间到' };
  const duration = Math.max(1000, Math.round(t.durationMs));
  const remaining = Math.min(duration, Math.max(1, Math.ceil(t.remainingMs / 1000) * 1000));
  return { duration_ms: duration, remaining_ms: remaining, active: t.running, title: label, detail: t.running ? label : '已暂停' };
}

/** elapsed_ms 0 also means "free-run a demo" on the device. */
export function stopwatchScene(t: { elapsedMs: number; running: boolean; label?: string }): ScenePayload {
  return { elapsed_ms: Math.max(1, Math.floor(t.elapsedMs / 100) * 100), active: t.running, title: t.label?.trim() || '秒表' };
}

/** The device shows whole minutes only, so the value changes once a minute. */
export function sleepTimerScene(t: { remainingMs: number; running: boolean }): ScenePayload {
  return { remaining_ms: Math.max(1, Math.ceil(t.remainingMs / 60000)) * 60000, active: t.running, title: '睡眠定时' };
}

export interface AlarmSceneInput { time: string; label: string; ringing?: boolean }
/** null when `time` is not HH:MM. The renderer treats hour 0 / minute 0 as
 *  "not given" (draws 07 / 30), so for those alarms the real time is repeated
 *  in the caption line until the device is fixed. */
export function alarmScene(a: AlarmSceneInput): ScenePayload | null {
  const m = /^(\d{1,2}):(\d{2})$/.exec(a.time);
  if (!m) return null;
  const hour = Number(m[1]), minute = Number(m[2]);
  if (hour > 23 || minute > 59) return null;
  const label = a.label.trim();
  const base = label || (a.ringing ? '闹钟响了' : '下一个闹钟');
  const title = hour === 0 || minute === 0 ? `${a.time} ${base}` : base;
  return { hour, minute, title, detail: title, alarm_copy: 'name', active: a.ringing === true };
}

export interface CalendarSceneInput { title: string; startAt: number; now?: number }
export function calendarCountdown(startAt: number, now = Date.now()): string {
  const day = (ms: number): number => { const d = new Date(ms); return Date.UTC(d.getFullYear(), d.getMonth(), d.getDate()) / 86400000; };
  const n = day(startAt) - day(now);
  return n === 0 ? '就是今天' : n === 1 ? '明天' : n > 1 ? `还有 ${n} 天` : `已过 ${-n} 天`;
}
/** Local wall-clock fields of the event; all-day events say so in `detail`
 *  because the renderer draws hour 0 / minute 0 as 09 / 30. */
export function calendarScene(e: CalendarSceneInput): ScenePayload {
  const d = new Date(e.startAt);
  const allDay = d.getHours() === 0 && d.getMinutes() === 0;
  const countdown = calendarCountdown(e.startAt, e.now);
  return {
    year: d.getFullYear(), month: d.getMonth() + 1, day: d.getDate(), hour: d.getHours(), minute: d.getMinutes(),
    title: e.title, detail: allDay ? `全天 · ${countdown}` : countdown,
  };
}

/* ---- information ------------------------------------------------------ */

const TASK_STATE: Record<string, string> = {
  running: 'running', queued: 'queued', confirm: 'confirm', done: 'done', failed: 'failed', cancelled: 'failed',
};
/** `progress` is 0..100 here and 0..1 on the wire; exactly 0 means "use the
 *  demo value" on the device, hence the floor. */
export function taskScene(t: { title: string; progress: number; state: string }): ScenePayload {
  const percent = Math.min(100, Math.max(0, Math.round(t.progress)));
  return { title: t.title, progress: Math.max(0.001, percent / 100), task_state: TASK_STATE[t.state] ?? 'queued', value: `${percent}%` };
}

/** A memory is one sentence: spread it over the two big lines, tag below. */
export function memoryScene(m: { text: string; tag: string }): ScenePayload {
  const [first = '', second = ''] = wrapText(m.text, 2);
  return { title: first, subtitle: second || m.tag, detail: second ? m.tag : '设备记忆' };
}

export function briefingScene(b: { index: number; count: number; title: string; source: string; playing: boolean }): ScenePayload {
  const index = Math.min(Math.max(1, b.count), Math.max(1, b.index + 1));
  return {
    briefing_index: index, briefing_count: Math.max(1, b.count), title: b.title, subtitle: b.source, playing: b.playing,
    ...(b.playing ? {} : { detail: `第 ${index} / ${Math.max(1, b.count)} 条` }),
  };
}

const COMPANION_MODE: Record<string, [string, string]> = {
  quiet: ['安静陪伴', '偶尔轻声问候'], interactive: ['适度互动', '结合日程与天气'], story: ['睡前陪伴', '一段温和的文字'],
};
export function companionScene(c: { enabled: boolean; mode: string; quietNow: boolean; dailyCount: number; dailyLimit: number }): ScenePayload {
  const [title, blurb] = COMPANION_MODE[c.mode] ?? COMPANION_MODE.quiet!;
  return {
    title, active: c.enabled && !c.quietNow,
    subtitle: !c.enabled ? '主动陪伴已关闭' : c.quietNow ? '免打扰中' : `今日 ${c.dailyCount}/${c.dailyLimit} 次`,
    detail: blurb,
  };
}

/* ---- system ------------------------------------------------------------ */

export interface HardwareSummary {
  audioDevices?: { available: boolean }[];
  storage?: { available: boolean }[];
  network?: { interfaces?: { up?: boolean; loopback?: boolean }[] };
}
export function hardwareCounts(s: HardwareSummary | null | undefined): { audio: number; storage: number; network: number; total: number } {
  const audio = s?.audioDevices?.filter(d => d.available).length ?? 0;
  const storage = s?.storage?.filter(d => d.available).length ?? 0;
  const network = s?.network?.interfaces?.filter(i => i.up === true && !i.loopback).length ?? 0;
  return { audio, storage, network, total: audio + storage + network };
}
/** The big figure is drawn with a Latin-only face, and a count of 0 falls back
 *  to a demo "2", so the number is always sent as the (ASCII) title too. */
export function devicesScene(s: HardwareSummary | null | undefined): ScenePayload {
  const c = hardwareCounts(s);
  return {
    device_count: Math.min(255, c.total), title: String(c.total), subtitle: '个硬件在线',
    detail: `音频 ${c.audio} · 存储 ${c.storage} · 网络 ${c.network}`,
  };
}

export function musicScene(m: { track: string; playing: boolean; elapsedMs: number; durationMs: number; lyrics?: { prev?: string; current?: string; next?: string } }): ScenePayload {
  const l = m.lyrics;
  const hasLyrics = !!(l && (l.prev?.trim() || l.current?.trim() || l.next?.trim()));
  return {
    title: m.track || '本地音乐', playing: m.playing, duration_ms: Math.max(0, m.durationMs),
    // 0 makes the device animate a demo position; whole seconds keep updates at 1 Hz.
    position_ms: Math.max(1, Math.floor(m.elapsedMs / 1000) * 1000),
    music_view: hasLyrics ? 'lyrics' : 'spectrum',
    ...(hasLyrics ? { previous_line: l!.prev ?? '', current_line: l!.current ?? '', next_line: l!.next ?? '' } : {}),
  };
}

/** `route` and `title` come from audio.status (lib/audioControl); without them only
 * `speaker` and `mute` can be told apart, which is all music.status knows. */
export function audioScene(a: { volume: number; muted: boolean; device: string; route?: 'speaker' | 'headphones' | 'both' | 'mute'; title?: string }): ScenePayload {
  const route = a.muted ? 'mute' : a.route ?? 'speaker';
  return { percent: route === 'mute' ? 0 : Math.round(Math.min(100, Math.max(0, a.volume))), title: a.title || a.device || '播放输出', audio_route: route };
}

export function networkScene(n: { ssid: string | null; rssi: number | null; connected: boolean }): ScenePayload {
  return {
    title: n.ssid ?? (n.connected ? '已连接' : '未连接'), network_state: n.connected ? 'wifi' : 'offline',
    signal_good: n.connected && (n.rssi === null || n.rssi >= -65),
    detail: n.rssi === null ? '' : `${n.rssi} dBm`,
  };
}

/** `title` is drawn with the Latin-only face: keep it ASCII. */
export function systemScene(s: { cpuPercent: number | null; memoryPercent: number | null; uptimeText: string; version: string }): ScenePayload {
  const cpu = s.cpuPercent === null ? null : Math.round(s.cpuPercent);
  const mem = s.memoryPercent === null ? null : Math.round(s.memoryPercent);
  return {
    title: cpu === null ? 'OPENVELA' : `CPU ${cpu}%`,
    subtitle: [mem === null ? '' : `内存 ${mem}%`, s.uptimeText ? `运行 ${s.uptimeText}` : ''].filter(Boolean).join(' · ') || '系统运行中',
    detail: s.version ? `Core ${s.version}` : '',
    percent: cpu ?? mem ?? 0,
  };
}

/** The sentence fills current/next; the sentence before it scrolls up. */
export function captionScene(text: string, previous = ''): ScenePayload {
  const [current = '', next = ''] = wrapText(text, 2);
  return { previous_line: previous, current_line: current, next_line: next };
}

export interface WeatherSceneInput {
  kind: string; city?: string; temperature: number; feelsLike?: number; humidity?: number; windKph?: number; visibilityKm?: number;
}
export function weatherScene(w: WeatherSceneInput): ScenePayload {
  return {
    weather: w.kind, title: w.city ?? '', temperature_c: w.temperature, feels_like_c: w.feelsLike ?? w.temperature,
    humidity_percent: w.humidity, wind_kph: w.windKph, visibility_km: w.visibilityKm,
  };
}
