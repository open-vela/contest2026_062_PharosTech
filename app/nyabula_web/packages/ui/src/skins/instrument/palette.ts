/* Instrument neutrals: near-black anodised panel (dark) / warm aluminium
 * (light). Flat, no tint; the preset color is the signal LED. */
import { derivePalette, mix, onColor } from '../../theme/derive';
import type { SkinPaletteFn } from '../../theme/types';

export const instrumentPalette: SkinPaletteFn = (preset) => {
  const base = derivePalette(preset);
  const { primary, primaryDeep } = preset;
  return {
    dark: {
      ...base.dark,
      primary,
      onPrimary: onColor(primary),
      primaryContainer: mix('#141416', primary, 0.22),
      onPrimaryContainer: primary,
      secondary: '#b9b9b3',
      secondaryContainer: '#232326',
      onSecondaryContainer: '#f2f2ee',
      tertiary: '#b9b9b3',
      surface: '#0b0b0c',
      surfaceDim: '#060607',
      surfaceContainer: '#141416',
      surfaceContainerHigh: '#1b1b1e',
      surfaceContainerHighest: '#242427',
      onSurface: '#e9e9e6',
      onSurfaceVariant: '#8a8a84',
      outline: '#48484b',
      outlineVariant: '#2a2a2c',
      error: '#ff5a3c',
      onError: '#0b0b0c',
      warning: '#ffb020',
      scrim: 'rgba(0,0,0,0.7)',
      glass: 'rgba(20,20,22,0.92)',
    },
    light: {
      ...base.light,
      primary: primaryDeep,
      onPrimary: onColor(primaryDeep),
      primaryContainer: mix('#f7f6f2', primary, 0.3),
      onPrimaryContainer: mix(primaryDeep, '#000000', 0.35),
      secondary: '#4a4a46',
      secondaryContainer: '#dcdbd5',
      onSecondaryContainer: '#111111',
      tertiary: '#4a4a46',
      surface: '#ecebe6',
      surfaceDim: '#d9d8d2',
      surfaceContainer: '#f6f5f1',
      surfaceContainerHigh: '#fbfaf7',
      surfaceContainerHighest: '#e1e0da',
      onSurface: '#111111',
      onSurfaceVariant: '#66655f',
      outline: '#a9a8a1',
      outlineVariant: '#cfcec8',
      error: '#e03a17',
      onError: '#ffffff',
      warning: '#b86e00',
      scrim: 'rgba(20,20,18,0.4)',
      glass: 'rgba(246,245,241,0.94)',
    },
  };
};
