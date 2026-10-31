/* What a skin contributes to the app. Shells replace the classic layout
 * for a form factor; pages replace a page entry (the component handles all
 * form factors itself, e.g. via <LayoutSwitch>). Anything omitted falls
 * back to classic. */
import type { Component } from 'vue';
import type { FormFactor } from '../composables/useFormFactor';

export type SkinPageId = 'home' | 'eye' | 'connect';

export interface AppSkin {
  shells?: Partial<Record<FormFactor, Component>>;
  pages?: Partial<Record<SkinPageId, Component>>;
}
