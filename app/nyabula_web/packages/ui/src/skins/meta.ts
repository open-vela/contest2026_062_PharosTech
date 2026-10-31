/* Skin meta + palette derivation (component-free, safe for the theme
 * store to import). */
import type { SkinPaletteFn, ThemeSkin } from '../theme/types';
import { nebulaPalette } from './nebula/palette';
import { instrumentPalette } from './instrument/palette';

export interface SkinMeta {
  id: ThemeSkin;
  name: string;
  desc: string;
}

/** Display order: default first. */
export const SKINS: SkinMeta[] = [
  { id: 'nebula', name: '星云', desc: '深空玻璃 · 猫眼为主角' },
  { id: 'classic', name: '经典', desc: 'Material 风格 · 规整' },
  { id: 'instrument', name: '仪器', desc: '点阵 HUD · 硬件感' },
];

export const DEFAULT_SKIN: ThemeSkin = 'classic';

export function isSkin(v: unknown): v is ThemeSkin {
  return SKINS.some((s) => s.id === v);
}

export const SKIN_PALETTES: Partial<Record<ThemeSkin, SkinPaletteFn>> = {
  nebula: nebulaPalette,
  instrument: instrumentPalette,
};
