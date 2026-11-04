import type { LightZone } from './lighting';

export interface LightingArtworkLayer {
  id: string; zone?: LightZone; pixel?: number;
  x: number; y: number; width: number; height: number;
  matrix: string; glow: string; color?: string;
}
function matrix(r: number, g: number, b: number) {
  return `${r} 0 0 0 0 0 ${g} 0 0 0 0 0 ${b} 0 0 0 0 0 1 0`;
}
function led(rgb: number) {
  const values = [(rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255];
  const peak = Math.max(...values);
  if (!peak) return { matrix: matrix(.12, .12, .12), glow: 'none', color: 'rgb(31,31,31)' };
  const level = .2 + .8 * Math.sqrt(peak / 255);
  const color = values.map(value => Math.round(value / peak * 255)).join(',');
  return { matrix: matrix(values[0] / peak * level, values[1] / peak * level, values[2] / peak * level),
    color: `rgb(${values.map(value => Math.round(value / peak * level * 255)).join(',')})`,
    glow: `drop-shadow(0 0 ${1 + level * 2}px rgba(${color},${level * .65}))` };
}

/** Coordinates refer to the supplied 360x480 light cutout. Both source PNGs
 * share the same 3:4 canvas; retain their original transparency and shading. */
export function lightingArtwork(pixels: readonly number[] = [], iris?: readonly string[]): LightingArtworkLayer[] {
  const layers: LightingArtworkLayer[] = [];
  const add = (zone: LightZone, pixel: number, x: number, y: number, width: number, height: number) =>
    layers.push({ id: `led-${pixel}`, zone, pixel, x, y, width, height, ...led(pixels[pixel] ?? 0) });
  add('left_ear', 0, 40, 25, 112, 125);
  add('right_ear', 1, 208, 25, 112, 125);
  for (let i = 0; i < 8; i++) add('chest', i + 2, 164, 318 + i * 10.25, 32, 10.55);
  for (let i = 0; i < 23; i++) add('base', i + 10, 64 + i * 232 / 23, 424, 232 / 23 + .3, 42);
  for (let eye = 0; eye < 2; eye++) {
    const hex = iris?.[eye];
    if (!hex || !/^#[\da-f]{6}$/i.test(hex)) continue;
    const rgb = Number.parseInt(hex.slice(1), 16);
    layers.push({ id: `eye-${eye}`, x: eye === 0 ? 77 : 203, y: 151, width: 80, height: 96,
      matrix: matrix(((rgb >> 16) & 255) / 255, ((rgb >> 8) & 255) / 255, (rgb & 255) / 255), glow: 'none' });
  }
  return layers;
}
