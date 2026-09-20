/* Theme model: mode (light/dark) x palette (brand color pair).
 * Ported from the Myself blog theme system; palettes derive a full MD3-like
 * token set from a single primary/deep color pair. */

export type ThemeMode = 'light' | 'dark';

/** A brand preset: only the two seed colors are authored by hand. */
export interface ThemePreset {
  id: string;
  /** Display name (zh-CN). */
  name: string;
  primary: string;
  primaryDeep: string;
}

/** Full derived token set for one mode. Keys map 1:1 onto CSS variables. */
export interface PaletteColors {
  primary: string;
  onPrimary: string;
  primaryContainer: string;
  onPrimaryContainer: string;
  secondary: string;
  onSecondary: string;
  secondaryContainer: string;
  onSecondaryContainer: string;
  tertiary: string;
  onTertiary: string;
  surface: string;
  surfaceDim: string;
  surfaceContainer: string;
  surfaceContainerHigh: string;
  surfaceContainerHighest: string;
  onSurface: string;
  onSurfaceVariant: string;
  outline: string;
  outlineVariant: string;
  error: string;
  onError: string;
  success: string;
  warning: string;
  scrim: string;
  /** "r,g,b" for rgba() composition. */
  primaryRgb: string;
  glass: string;
  primaryDeep: string;
}

export interface Palette {
  id: string;
  name: string;
  light: PaletteColors;
  dark: PaletteColors;
}
