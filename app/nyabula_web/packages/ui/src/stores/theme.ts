/* Theme store: mode x palette, persisted, follows the OS scheme until the
 * user picks one explicitly. Writes derived tokens onto :root. */
import { defineStore } from 'pinia';
import { derivePalette, mix, TOKEN_VARS } from '../theme/derive';
import { DEFAULT_PRESET_ID, THEME_PRESETS } from '../theme/presets';
import type { Palette, ThemeMode, ThemePreset } from '../theme/types';
import { applyFavicon } from '../faviconTemplates';

const STORAGE_KEY = 'nyabula.theme';

interface Persisted {
  mode: ThemeMode | '';
  paletteId: string;
}

function loadPersisted(): Persisted {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) return JSON.parse(raw) as Persisted;
  } catch {
    /* ignore corrupt data */
  }
  return { mode: '', paletteId: '' };
}

function applyToRoot(palette: Palette, mode: ThemeMode): void {
  const root = document.documentElement;
  const colors = palette[mode];
  root.dataset.theme = mode;
  root.dataset.mode = mode;
  root.dataset.palette = palette.id;
  root.style.colorScheme = mode;
  for (const key of Object.keys(TOKEN_VARS) as (keyof typeof TOKEN_VARS)[]) {
    root.style.setProperty(TOKEN_VARS[key], colors[key]);
  }
  // Logo layers step off the page surface: a touch darker in light mode,
  // a touch lighter in dark mode. No glow, no shadow; hierarchy by tone.
  const dark = mode === 'dark';
  root.style.setProperty('--logo-bg', dark ? mix(colors.surface, '#000000', 0.5) : '#ffffff');
  root.style.setProperty('--logo-shade', dark ? mix(colors.surface, '#ffffff', 0.22) : mix(colors.surface, '#000000', 0.14));
  root.style.setProperty('--logo-ink', dark ? '#f6f6f3' : '#111418');
  root.style.setProperty('--logo-nose', colors.primary);
  applyFavicon(mode, colors.primary);
}

export const useThemeStore = defineStore('nyabula-theme', {
  state: () => ({
    mode: 'dark' as ThemeMode,
    paletteId: DEFAULT_PRESET_ID,
    /** True when the user picked a mode; otherwise we follow the OS. */
    explicitMode: false,
    presets: THEME_PRESETS as ThemePreset[],
    initialized: false,
  }),
  getters: {
    palettes(): Palette[] {
      return this.presets.map(derivePalette);
    },
    palette(): Palette {
      return this.palettes.find((p) => p.id === this.paletteId) ?? this.palettes[0]!;
    },
  },
  actions: {
    init() {
      if (this.initialized) return;
      const saved = loadPersisted();
      const mq = window.matchMedia('(prefers-color-scheme: dark)');
      this.explicitMode = saved.mode === 'light' || saved.mode === 'dark';
      this.mode = this.explicitMode ? (saved.mode as ThemeMode) : mq.matches ? 'dark' : 'light';
      this.paletteId = this.presets.some((p) => p.id === saved.paletteId) ? saved.paletteId : DEFAULT_PRESET_ID;
      mq.addEventListener('change', (e) => {
        if (!this.explicitMode) {
          this.mode = e.matches ? 'dark' : 'light';
          applyToRoot(this.palette, this.mode);
        }
      });
      this.initialized = true;
      applyToRoot(this.palette, this.mode);
    },
    setMode(mode: ThemeMode) {
      this.mode = mode;
      this.explicitMode = true;
      this.persistAndApply();
    },
    toggleMode() {
      this.setMode(this.mode === 'light' ? 'dark' : 'light');
    },
    /** Return to following the OS scheme. */
    followSystem() {
      this.explicitMode = false;
      this.mode = window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
      this.persistAndApply();
    },
    setPalette(id: string) {
      if (!this.presets.some((p) => p.id === id)) return;
      this.paletteId = id;
      this.persistAndApply();
    },
    /** Hosts may register extra presets (e.g. plugin-provided brands). */
    registerPreset(preset: ThemePreset) {
      if (!this.presets.some((p) => p.id === preset.id)) this.presets.push(preset);
    },
    persistAndApply() {
      localStorage.setItem(
        STORAGE_KEY,
        JSON.stringify({ mode: this.explicitMode ? this.mode : '', paletteId: this.paletteId } satisfies Persisted),
      );
      applyToRoot(this.palette, this.mode);
    },
  },
});
