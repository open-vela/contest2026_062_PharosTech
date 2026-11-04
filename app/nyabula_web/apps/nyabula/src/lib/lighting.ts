export const LIGHT_ZONES = [
  { id: 'left_ear', name: '左耳', count: 1, first: 0 },
  { id: 'right_ear', name: '右耳', count: 1, first: 1 },
  { id: 'chest', name: '胸口', count: 8, first: 2 },
  { id: 'base', name: '底座', count: 23, first: 10 },
] as const;
export type LightZone = typeof LIGHT_ZONES[number]['id'];
export type LightEffect = 'steady' | 'breathe' | 'heartbeat' | 'aurora' | 'orbit' | 'music' | 'status';
export type LightColorMode = 'fixed' | 'iris' | 'complement';
export interface LightGroup { id: LightZone; count: number; enabled: boolean; color: string; brightness: number; speed: number; effect: LightEffect; colorMode: LightColorMode; outputColor: string }
export interface LightingStatus { available: boolean; running: boolean; sleeping: boolean; phase: string; preset: string; activity: string; maxBrightness: number; revision: number; appliedRevision: number; frames: number; lastWriteMs: number; lastError: number; musicActive: boolean; musicLevel: number; musicSource: string; groups: LightGroup[]; pixels: number[] }
export interface LightChange { enabled?: boolean; color?: string; brightness?: number; brightnessDelta?: number; effect?: LightEffect; colorMode?: LightColorMode; speed?: number }
export const LIGHT_EFFECTS: { id: LightEffect; name: string; detail: string }[] = [
  { id: 'steady', name: '常亮', detail: '干净、稳定的光' },
  { id: 'breathe', name: '呼吸', detail: '缓缓亮起，再轻轻落下' },
  { id: 'heartbeat', name: '心跳', detail: '两次轻跳，一段安静' },
  { id: 'aurora', name: '极光', detail: '互补色沿底座缓慢流动' },
  { id: 'orbit', name: '环绕', detail: '一束柔光绕底座行走' },
  { id: 'music', name: '音乐律动', detail: '跟随实际播放电平，停播归黑' },
  { id: 'status', name: '状态', detail: '聆听蓝、思考紫、说话青绿' },
];
export const LIGHT_PRESETS = [
  { id: 'calm', name: '静谧', detail: '暖光呼吸 · 胸口心跳', icon: 'moon', colors: ['#ffbf8c', '#ffad69'] },
  { id: 'aurora', name: '极光', detail: '瞳色互补 · 底座流光', icon: 'star', colors: ['#638cff', '#d79aff'] },
  { id: 'focus', name: '专注', detail: '低亮暖光 · 胸口状态', icon: 'tune', colors: ['#ffd29a', '#6fd3ff'] },
  { id: 'party', name: '随乐', detail: '四区跟随真实音乐', icon: 'music_note', colors: ['#5ccfff', '#ff78a1'] },
] as const;
export function effectsFor(zone: LightZone) { return LIGHT_EFFECTS.filter(effect => (!['aurora', 'orbit'].includes(effect.id) || zone === 'base') && (effect.id !== 'status' || zone === 'chest')); }
export function lightingStatus(value: unknown): value is LightingStatus {
  const s = value as LightingStatus | null;
  return !!s && typeof s.available === 'boolean' && Number.isFinite(s.revision) &&
    Number.isFinite(s.maxBrightness) && s.maxBrightness > 0 && s.maxBrightness <= 100 &&
    Array.isArray(s.groups) && s.groups.length === 4 && LIGHT_ZONES.every(zone => s.groups.filter(g => g.id === zone.id).length === 1) &&
    s.groups.every(g => /^#[0-9a-f]{6}$/i.test(g.color) && Number.isFinite(g.brightness) && g.brightness >= 0 && g.brightness <= s.maxBrightness && typeof g.enabled === 'boolean') &&
    Array.isArray(s.pixels) && s.pixels.length === 33 && s.pixels.every(n => Number.isInteger(n) && n >= 0 && n <= 0xffffff);
}
/** Lift the sent color for legibility while retaining its hue and relative level. */
export function lightDot(rgb: number) {
  const values = [(rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255];
  const peak = Math.max(...values);
  return { color: peak ? `rgb(${values.map(n => Math.round(n / peak * 255)).join(',')})` : 'var(--md-outline-variant)', opacity: peak ? .3 + .7 * Math.sqrt(Math.min(1, peak / 255)) : .2 };
}
