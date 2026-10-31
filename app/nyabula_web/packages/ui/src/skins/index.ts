/* Skin registry: boot overlay, route curtain and theme-switch motion per
 * skin (meta + palettes live in ./meta so the theme store can import them
 * without pulling components). Loading components keep the loading-store
 * contract (see skins/classic/*): boot exits after bootDone and clears
 * bootOverlayVisible; the curtain calls markCovered() once the old page is
 * hidden and clears routeOverlayVisible after its exit. */
import type { Component } from 'vue';
import type { ThemeSkin } from '../theme/types';
import type { SkinTransition } from './types';
import { circularReveal } from '../theme/circularReveal';
import { DEFAULT_SKIN, isSkin } from './meta';
import ClassicBoot from './classic/ClassicBoot.vue';
import ClassicCurtain from './classic/ClassicCurtain.vue';
import NebulaBoot from './nebula/NebulaBoot.vue';
import NebulaCurtain from './nebula/NebulaCurtain.vue';
import InstrumentBoot from './instrument/InstrumentBoot.vue';
import InstrumentCurtain from './instrument/InstrumentCurtain.vue';
import { nebulaTransition } from './nebula/transition';
import { instrumentTransition } from './instrument/transition';

export type { SkinTransition } from './types';
export { SKINS, DEFAULT_SKIN, isSkin, SKIN_PALETTES } from './meta';
export type { SkinMeta } from './meta';

export const SKIN_BOOT: Record<ThemeSkin, Component> = { classic: ClassicBoot, nebula: NebulaBoot, instrument: InstrumentBoot };
export const SKIN_CURTAIN: Record<ThemeSkin, Component> = { classic: ClassicCurtain, nebula: NebulaCurtain, instrument: InstrumentCurtain };

const TRANSITIONS: Record<ThemeSkin, SkinTransition> = {
  classic: circularReveal,
  nebula: nebulaTransition,
  instrument: instrumentTransition,
};

/** Current skin as applied on <html data-skin>. */
export function currentSkin(): ThemeSkin {
  const s = document.documentElement.dataset.skin;
  return isSkin(s) ? s : DEFAULT_SKIN;
}

/**
 * Full-screen theme change (mode / palette / skin) using the motion of
 * `skin` (defaults to the current one; pass the target skin when switching
 * skins so the new look introduces itself).
 */
export function themeTransition(
  origin: { x: number; y: number },
  apply: () => void,
  direction: 'expand' | 'contract' = 'expand',
  skin: ThemeSkin = currentSkin(),
): void {
  TRANSITIONS[skin](origin, apply, direction);
}
