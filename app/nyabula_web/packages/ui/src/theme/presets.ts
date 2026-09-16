import type { ThemePreset } from './types';

/** Built-in brand palettes. "mint" matches the original MD3 seed #62DFAF. */
export const THEME_PRESETS: ThemePreset[] = [
  { id: 'mint', name: '薄荷', primary: '#62dfaf', primaryDeep: '#006c4c' },
  { id: 'sakura', name: '樱花', primary: '#ff8fb1', primaryDeep: '#b3255a' },
  { id: 'ocean', name: '海洋', primary: '#6fb8ff', primaryDeep: '#0b5cad' },
  { id: 'amber', name: '琥珀', primary: '#ffc857', primaryDeep: '#9a5b00' },
];

export const DEFAULT_PRESET_ID = 'mint';
