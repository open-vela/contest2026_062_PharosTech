/* Derive a complete light+dark token set from a two-color preset.
 * Neutral surfaces are lightly tinted with the primary so every palette
 * reads as one system; text/outline follow luminance. */
import type { Palette, PaletteColors, ThemePreset } from './types';

function hexToRgb(hex: string): [number, number, number] {
  const v = hex.replace('#', '');
  return [parseInt(v.slice(0, 2), 16), parseInt(v.slice(2, 4), 16), parseInt(v.slice(4, 6), 16)];
}

function toHex(rgb: [number, number, number]): string {
  return `#${rgb.map((c) => Math.round(Math.min(255, Math.max(0, c))).toString(16).padStart(2, '0')).join('')}`;
}

/** Linear mix a -> b by ratio in sRGB. */
export function mix(a: string, b: string, ratio: number): string {
  const ra = hexToRgb(a);
  const rb = hexToRgb(b);
  return toHex([0, 1, 2].map((i) => ra[i] + (rb[i] - ra[i]) * ratio) as [number, number, number]);
}

function luminance(hex: string): number {
  const [r, g, b] = hexToRgb(hex).map((c) => {
    const s = c / 255;
    return s <= 0.03928 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

/** Pick black or white text for a background. */
export function onColor(bg: string): string {
  return luminance(bg) > 0.4 ? '#0e1512' : '#ffffff';
}

export function derivePalette(preset: ThemePreset): Palette {
  const { primary, primaryDeep } = preset;
  const rgb = hexToRgb(primary).join(',');

  const light: PaletteColors = {
    primary: primaryDeep,
    onPrimary: onColor(primaryDeep),
    primaryContainer: mix(primary, '#ffffff', 0.55),
    onPrimaryContainer: mix(primaryDeep, '#000000', 0.55),
    secondary: mix(primaryDeep, '#5c6270', 0.55),
    onSecondary: '#ffffff',
    secondaryContainer: mix(primary, '#e6eaef', 0.72),
    onSecondaryContainer: mix(primaryDeep, '#1b1f26', 0.6),
    tertiary: mix(primaryDeep, '#3d6472', 0.5),
    onTertiary: '#ffffff',
    surface: mix('#f7f9fa', primary, 0.04),
    surfaceDim: mix('#d8dcdf', primary, 0.06),
    surfaceContainer: mix('#eceff2', primary, 0.06),
    surfaceContainerHigh: mix('#e4e8ec', primary, 0.07),
    surfaceContainerHighest: mix('#dde2e6', primary, 0.08),
    onSurface: mix('#17181c', primary, 0.08),
    onSurfaceVariant: mix('#5c6270', primary, 0.14),
    outline: mix('#727a86', primary, 0.12),
    outlineVariant: mix('#c3c9d2', primary, 0.12),
    error: '#ba1a1a',
    onError: '#ffffff',
    success: primaryDeep,
    warning: '#9a6b00',
    scrim: 'rgba(0,0,0,0.32)',
    primaryRgb: rgb,
    glass: 'rgba(255,255,255,0.55)',
    primaryDeep,
  };

  const dark: PaletteColors = {
    primary,
    onPrimary: mix(primaryDeep, '#000000', 0.6),
    primaryContainer: mix(primaryDeep, '#000000', 0.25),
    onPrimaryContainer: mix(primary, '#ffffff', 0.35),
    secondary: mix(primary, '#b3bcc6', 0.6),
    onSecondary: '#1f2a35',
    secondaryContainer: mix('#354049', primary, 0.16),
    onSecondaryContainer: mix('#d3dde6', primary, 0.12),
    tertiary: mix(primary, '#a4cddf', 0.5),
    onTertiary: '#063543',
    surface: mix('#0f1214', primary, 0.05),
    surfaceDim: mix('#0a0c0e', primary, 0.04),
    surfaceContainer: mix('#1a1e21', primary, 0.06),
    surfaceContainerHigh: mix('#23282c', primary, 0.07),
    surfaceContainerHighest: mix('#2d3337', primary, 0.08),
    onSurface: mix('#e2e6e8', primary, 0.04),
    onSurfaceVariant: mix('#8d959d', primary, 0.1),
    outline: mix('#5b646c', primary, 0.12),
    outlineVariant: mix('#3d454c', primary, 0.12),
    error: '#ffb4ab',
    onError: '#690005',
    success: primary,
    warning: '#ffd94d',
    scrim: 'rgba(0,0,0,0.55)',
    primaryRgb: rgb,
    glass: 'rgba(20,24,28,0.55)',
    primaryDeep,
  };

  return { id: preset.id, name: preset.name, light, dark };
}

/** CSS variable name for each PaletteColors key. */
export const TOKEN_VARS: Record<keyof PaletteColors, string> = {
  primary: '--md-primary',
  onPrimary: '--md-on-primary',
  primaryContainer: '--md-primary-container',
  onPrimaryContainer: '--md-on-primary-container',
  secondary: '--md-secondary',
  onSecondary: '--md-on-secondary',
  secondaryContainer: '--md-secondary-container',
  onSecondaryContainer: '--md-on-secondary-container',
  tertiary: '--md-tertiary',
  onTertiary: '--md-on-tertiary',
  surface: '--md-surface',
  surfaceDim: '--md-surface-dim',
  surfaceContainer: '--md-surface-container',
  surfaceContainerHigh: '--md-surface-container-high',
  surfaceContainerHighest: '--md-surface-container-highest',
  onSurface: '--md-on-surface',
  onSurfaceVariant: '--md-on-surface-variant',
  outline: '--md-outline',
  outlineVariant: '--md-outline-variant',
  error: '--md-error',
  onError: '--md-on-error',
  success: '--md-success',
  warning: '--md-warning',
  scrim: '--md-scrim',
  primaryRgb: '--md-primary-rgb',
  glass: '--md-glass',
  primaryDeep: '--md-primary-deep',
};
