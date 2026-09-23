/* Local (this browser) settings shared by all three variants: theme mode,
 * palette, form-factor override, developer mode, clear local data. */
import { computed, inject, ref } from 'vue';
import { circularReveal, useDialogStore, useThemeStore, useToastStore, type ThemeMode } from '@nyabula/ui';
import type { FormFactor, useFormFactor } from '../../composables/useFormFactor';

import { useDevStore, DEV_KEY } from '../../stores/dev';
export { DEV_KEY };
const CLEAR_PREFIXES = ['nyabula.', 'nyalink.'];
/** Keep the theme so the page does not flash after a data wipe. */
const CLEAR_KEEP = new Set(['nyabula.theme', 'nyabula.formFactor']);

export type ModeChoice = 'system' | ThemeMode;

export function useSettingsPage() {
  const theme = useThemeStore();
  const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
  const dialog = useDialogStore();
  const toast = useToastStore();

  /* Theme mode */
  const modeChoices: { id: ModeChoice; label: string; icon: string }[] = [
    { id: 'system', label: '跟随系统', icon: 'settings' },
    { id: 'light', label: '浅色', icon: 'light_mode' },
    { id: 'dark', label: '深色', icon: 'dark_mode' },
  ];
  const mode = computed<ModeChoice>(() => (theme.explicitMode ? theme.mode : 'system'));
  function setMode(choice: ModeChoice, origin?: { x: number; y: number }): void {
    if (choice === mode.value) return;
    const before = theme.mode;
    const apply = () => (choice === 'system' ? theme.followSystem() : theme.setMode(choice));
    if (!origin) return apply();
    // Direction hint: going lighter expands, going darker contracts.
    const after = choice === 'system' ? (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light') : choice;
    circularReveal(origin, apply, before === 'dark' && after === 'light' ? 'expand' : after === 'dark' && before === 'light' ? 'contract' : 'expand');
  }

  /* Palette */
  const palettes = computed(() => theme.palettes);
  const paletteId = computed(() => theme.paletteId);
  function setPalette(id: string, origin?: { x: number; y: number }): void {
    if (id === theme.paletteId) return;
    if (origin) circularReveal(origin, () => theme.setPalette(id), 'expand');
    else theme.setPalette(id);
  }

  /* Layout override */
  const layoutChoices: { id: FormFactor | 'auto'; label: string; icon: string }[] = [
    { id: 'auto', label: '自动', icon: 'auto_awesome' },
    { id: 'desktop', label: '桌面', icon: 'desktop' },
    { id: 'tablet', label: '平板', icon: 'tablet' },
    { id: 'phone', label: '手机', icon: 'smartphone' },
  ];
  const layout = computed<FormFactor | 'auto'>(() => ff.override.value ?? 'auto');
  const layoutAuto = computed(() => ff.auto.value);
  function setLayout(v: FormFactor | 'auto'): void {
    ff.setOverride(v === 'auto' ? null : v);
  }

  /* Developer mode */
  const devStore = useDevStore();
  const dev = computed(() => devStore.enabled);
  function setDev(v: boolean): void {
    devStore.enabled = v;
    toast.ok(v ? '开发模式已开启：未连接也可浏览设备页面' : '开发模式已关闭');
  }

  /* Clear local data */
  const localCount = computed(() => countLocal());
  function countLocal(): number {
    let n = 0;
    try {
      for (let i = 0; i < localStorage.length; i++) {
        const k = localStorage.key(i) ?? '';
        if (CLEAR_PREFIXES.some((p) => k.startsWith(p)) && !CLEAR_KEEP.has(k)) n++;
      }
    } catch {
      /* ignore */
    }
    return n;
  }
  async function clearLocal(): Promise<void> {
    const ok = await dialog.confirm('将删除本机保存的已知设备、配对令牌与账号会话，页面随后会重新加载。主题与布局设置保留。', {
      title: '清除本机数据？',
      danger: true,
      confirmText: '清除并重载',
    });
    if (!ok) return;
    const keys: string[] = [];
    for (let i = 0; i < localStorage.length; i++) {
      const k = localStorage.key(i);
      if (k && CLEAR_PREFIXES.some((p) => k.startsWith(p)) && !CLEAR_KEEP.has(k)) keys.push(k);
    }
    keys.forEach((k) => localStorage.removeItem(k));
    location.reload();
  }

  return { theme, modeChoices, mode, setMode, palettes, paletteId, setPalette, layoutChoices, layout, layoutAuto, setLayout, dev, setDev, localCount, clearLocal };
}
