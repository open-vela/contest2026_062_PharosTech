/* Injection context shared between NyaUiPage (provider) and NyaUiNode (consumer). */
import type { InjectionKey } from 'vue';
import type { NyaUiNode } from './types.js';

export interface NyaUiContext {
  /** Current effective value for an input component id (optimistic overlay). */
  getValue: (id: string | undefined) => unknown;
  /** Record a local edit. `immediate` fires the change event now (slider
   * release, switch, chips); otherwise it is debounced 300 ms per component. */
  setValue: (node: NyaUiNode, value: unknown, immediate: boolean) => void;
  /** Fire a non-change event (e.g. button tap, listTile tap). Applies the
   * native-confirm guard when `props.confirm` is a string. `extra` is merged
   * into the resolved command args (kit components attach event payloads such
   * as the seek position or tapped contact id this way). */
  fire: (node: NyaUiNode, event: string, extra?: Record<string, unknown>) => void;
}

export const NYAUI_CTX: InjectionKey<NyaUiContext> = Symbol('nyaui-ctx');
