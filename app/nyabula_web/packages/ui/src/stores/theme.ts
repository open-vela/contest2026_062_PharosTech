/* Theme store: skin x mode x palette, persisted, follows the OS scheme until
 * the user picks one explicitly. Writes derived tokens onto :root. */
import { defineStore } from 'pinia';
import { derivePalette, mix, TOKEN_VARS } from '../theme/derive';
import { DEFAULT_PRESET_ID, THEME_PRESETS } from '../theme/presets';
import type { Palette, ThemeMode, ThemePreset, ThemeSkin } from '../theme/types';
import { DEFAULT_SKIN, isSkin, SKIN_PALETTES, SKINS } from '../skins/meta';
import { applyFavicon } from '../faviconTemplates';

const STORAGE_KEY = 'nyabula.theme';

interface Persisted {
  mode: ThemeMode | '';
  paletteId: string;
  skin?: ThemeSkin;
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

function paletteFor(preset: ThemePreset, skin: ThemeSkin): Palette {
  const fn = SKIN_PALETTES[skin];
  if (!fn) return derivePalette(preset);
  return { id: preset.id, name: preset.name, ...fn(preset) };
}

function applyToRoot(palette: Palette, mode: ThemeMode, skin: ThemeSkin): void {
  const root = document.documentElement;
  const colors = palette[mode];
  root.dataset.theme = mode;
  root.dataset.mode = mode;
  root.dataset.palette = palette.id;
  root.dataset.skin = skin;
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
    skin: DEFAULT_SKIN as ThemeSkin,
    /** True when the user picked a mode; otherwise we follow the OS. */
    explicitMode: false,
    presets: THEME_PRESETS as ThemePreset[],
    skins: SKINS,
    initialized: false,
  }),
  getters: {
    palettes(): Palette[] {
      return this.presets.map((p) => paletteFor(p, this.skin));
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
      this.skin = isSkin(saved.skin) ? saved.skin : DEFAULT_SKIN;
      mq.addEventListener('change', (e) => {
        if (!this.explicitMode) {
          this.mode = e.matches ? 'dark' : 'light';
          applyToRoot(this.palette, this.mode, this.skin);
        }
      });
      this.initialized = true;
      if (saved.skin && !isSkin(saved.skin)) this.persistAndApply();
      else applyToRoot(this.palette, this.mode, this.skin);
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
    setSkin(skin: ThemeSkin) {
      if (!isSkin(skin)) return;
      this.skin = skin;
      this.persistAndApply();
    },
    /** Hosts may register extra presets (e.g. plugin-provided brands). */
    registerPreset(preset: ThemePreset) {
      if (!this.presets.some((p) => p.id === preset.id)) this.presets.push(preset);
    },
    persistAndApply() {
      localStorage.setItem(
        STORAGE_KEY,
        JSON.stringify({ mode: this.explicitMode ? this.mode : '', paletteId: this.paletteId, skin: this.skin } satisfies Persisted),
      );
      applyToRoot(this.palette, this.mode, this.skin);
    },
  },
});
