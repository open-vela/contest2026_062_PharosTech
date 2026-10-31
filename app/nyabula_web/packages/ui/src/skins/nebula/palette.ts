/* Nebula neutrals: deep-space navy (dark) / pearl dawn (light). Brand color
 * from the preset. Container surfaces are translucent so every page's
 * cards read as glass over the aurora backdrop; the page surface itself
 * stays opaque (it is what the backdrop paints over). */
import { derivePalette, mix, onColor } from '../../theme/derive';
import type { SkinPaletteFn } from '../../theme/types';

function rgba(hex: string, a: number): string {
  const v = hex.replace('#', '');
  const [r, g, b] = [0, 2, 4].map((i) => parseInt(v.slice(i, i + 2), 16));
  return `rgba(${r},${g},${b},${a})`;
}

export const nebulaPalette: SkinPaletteFn = (preset) => {
  const base = derivePalette(preset);
  const { primary, primaryDeep } = preset;
  return {
    dark: {
      ...base.dark,
      primary,
      onPrimary: onColor(primary),
      primaryContainer: rgba(primary, 0.2),
      onPrimaryContainer: mix(primary, '#ffffff', 0.35),
      secondary: mix(primary, '#b8c0ff', 0.55),
      secondaryContainer: rgba(primary, 0.16),
      onSecondaryContainer: mix('#eef1ff', primary, 0.25),
      tertiary: '#a99cff',
      onTertiary: '#150f3d',
      surface: mix('#070918', primary, 0.025),
      surfaceDim: '#04050d',
      surfaceContainer: 'rgba(168,182,255,0.055)',
      surfaceContainerHigh: 'rgba(168,182,255,0.09)',
      surfaceContainerHighest: 'rgba(180,192,255,0.14)',
      onSurface: '#eef1ff',
      onSurfaceVariant: '#9aa2c6',
      outline: '#5a6290',
      outlineVariant: 'rgba(170,184,255,0.14)',
      success: primary,
      warning: '#ffd27a',
      error: '#ff9aa8',
      onError: '#3a0710',
      scrim: 'rgba(3,4,12,0.55)',
      glass: 'rgba(16,20,42,0.62)',
    },
    light: {
      ...base.light,
      primary: primaryDeep,
      onPrimary: onColor(primaryDeep),
      primaryContainer: rgba(primary, 0.24),
      onPrimaryContainer: mix(primaryDeep, '#000000', 0.45),
      secondary: mix(primaryDeep, '#4b5280', 0.5),
      secondaryContainer: rgba(primaryDeep, 0.12),
      onSecondaryContainer: mix('#161a38', primaryDeep, 0.35),
      tertiary: '#5b4fd6',
      onTertiary: '#ffffff',
      surface: mix('#f2f3fb', primary, 0.03),
      surfaceDim: '#dcdff0',
      surfaceContainer: 'rgba(255,255,255,0.58)',
      surfaceContainerHigh: 'rgba(255,255,255,0.74)',
      surfaceContainerHighest: 'rgba(226,230,248,0.9)',
      onSurface: '#141833',
      onSurfaceVariant: '#565d82',
      outline: '#8a90b0',
      outlineVariant: 'rgba(40,48,110,0.12)',
      success: primaryDeep,
      warning: '#a86a00',
      scrim: 'rgba(24,28,60,0.28)',
      glass: 'rgba(255,255,255,0.66)',
    },
  };
};
