/* Appearance controls shared with the device WebUI (same localStorage key
 * `nyabula.theme`): skin, light/dark (or follow system), palette. Every
 * change runs through the skin's full-screen theme transition; a skin
 * change uses the target skin's motion. */
import { computed } from 'vue';
import { themeTransition, useThemeStore, type ThemeMode, type ThemeSkin } from '@nyabula/ui';

export type ModeChoice = 'system' | ThemeMode;
type Origin = { x: number; y: number };

export function useAppearance() {
  const theme = useThemeStore();

  const modeChoices: { id: ModeChoice; label: string; icon: string }[] = [
    { id: 'system', label: '跟随系统', icon: 'settings' },
    { id: 'light', label: '浅色', icon: 'light_mode' },
    { id: 'dark', label: '深色', icon: 'dark_mode' },
  ];
  const mode = computed<ModeChoice>(() => (theme.explicitMode ? theme.mode : 'system'));
  function setMode(choice: ModeChoice, origin?: Origin): void {
    if (choice === mode.value) return;
    const before = theme.mode;
    const apply = () => (choice === 'system' ? theme.followSystem() : theme.setMode(choice));
    if (!origin) return apply();
    const after = choice === 'system' ? (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light') : choice;
    // Going lighter expands, going darker contracts.
    themeTransition(origin, apply, before === 'dark' && after === 'light' ? 'expand' : after === 'dark' && before === 'light' ? 'contract' : 'expand');
  }
  function toggleMode(origin?: Origin): void {
    setMode(theme.mode === 'dark' ? 'light' : 'dark', origin);
  }

  const palettes = computed(() => theme.palettes);
  const paletteId = computed(() => theme.paletteId);
  function setPalette(id: string, origin?: Origin): void {
    if (id === theme.paletteId) return;
    if (origin) themeTransition(origin, () => theme.setPalette(id), 'expand');
    else theme.setPalette(id);
  }

  const skins = computed(() => theme.skins);
  const skin = computed<ThemeSkin>(() => theme.skin);
  function setSkin(id: ThemeSkin, origin?: Origin): void {
    if (id === theme.skin) return;
    if (origin) themeTransition(origin, () => theme.setSkin(id), 'expand', id);
    else theme.setSkin(id);
  }

  return { theme, modeChoices, mode, setMode, toggleMode, palettes, paletteId, setPalette, skins, skin, setSkin };
}
