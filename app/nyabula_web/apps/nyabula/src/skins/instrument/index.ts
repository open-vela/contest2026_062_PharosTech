/* Instrument skin: shells + page overrides (see ../types.ts). Visual
 * language: dot-matrix readouts, numbered hardware keys, LEDs, hairline
 * panels. Component styling lives in @nyabula/ui skins/instrument. */
import type { AppSkin } from '../types';
import InstrumentDesktopShell from './shells/InstrumentDesktopShell.vue';
import InstrumentTabletShell from './shells/InstrumentTabletShell.vue';
import InstrumentPhoneShell from './shells/InstrumentPhoneShell.vue';
import InstrumentHome from './pages/InstrumentHome.vue';
import InstrumentEye from './pages/InstrumentEye.vue';
import InstrumentConnect from './pages/InstrumentConnect.vue';

export const instrumentSkin: AppSkin = {
  shells: { desktop: InstrumentDesktopShell, tablet: InstrumentTabletShell, phone: InstrumentPhoneShell },
  pages: { home: InstrumentHome, eye: InstrumentEye, connect: InstrumentConnect },
};
