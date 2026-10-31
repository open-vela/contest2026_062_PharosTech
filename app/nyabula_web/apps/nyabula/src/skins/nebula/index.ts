/* Nebula skin (design-lab A): one glass shell for every form factor plus
 * eye-first home, eye and connect pages (see ../types.ts). Everything else
 * is styled by packages/ui/src/skins/nebula/nebula.css. */
import type { AppSkin } from '../types';
import NebulaShell from './NebulaShell.vue';
import NebulaHome from './home/NebulaHome.vue';
import NebulaEye from './eye/NebulaEye.vue';
import NebulaConnect from './connect/NebulaConnect.vue';

export const nebulaSkin: AppSkin = {
  shells: { desktop: NebulaShell, tablet: NebulaShell, phone: NebulaShell },
  pages: { home: NebulaHome, eye: NebulaEye, connect: NebulaConnect },
};
