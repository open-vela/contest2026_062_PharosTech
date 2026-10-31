/* App skin registry keyed by ThemeSkin; classic is the built-in layout. */
import type { ThemeSkin } from '@nyabula/ui';
import type { AppSkin } from './types';
import { nebulaSkin } from './nebula';
import { instrumentSkin } from './instrument';

export const APP_SKINS: Record<ThemeSkin, AppSkin> = {
  classic: {},
  nebula: nebulaSkin,
  instrument: instrumentSkin,
};
