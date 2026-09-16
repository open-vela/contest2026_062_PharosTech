/* NyaUI DSL v1 types. Contract: Shared/nyaui/nyaui.md (single source of truth). */

export interface NyaUiAction {
  command?: string;
  /** Keys starting with `$` reference another component id's current value. */
  args?: Record<string, unknown>;
}

export interface NyaUiNode {
  type: string;
  id?: string;
  props?: Record<string, unknown>;
  children?: NyaUiNode[];
  on?: Record<string, NyaUiAction>;
}

export interface NyaUiPageMeta {
  title?: string;
  icon?: string;
  placement?: 'menu' | 'standalone';
}

export interface NyaUiTree {
  nyaui: number;
  page?: NyaUiPageMeta;
  root: NyaUiNode;
}

export interface NyaUiPatch {
  /** JSON Pointer relative to the tree document root, e.g. `/root/children/2/props/value`. */
  path: string;
  value: unknown;
}

/** Payload emitted by NyaUiPage when a component fires an event. */
export interface NyaUiCommand {
  componentId?: string;
  event: string;
  command?: string;
  args?: Record<string, unknown>;
  /** Snapshot of every id-bearing input component's current value on the page. */
  values: Record<string, unknown>;
}

/** Container types — the only ones allowed to carry `children`. */
export const CONTAINER_TYPES = new Set(['column', 'row', 'grid', 'card', 'section', 'listSection']);

/** Input types with value semantics (contribute to the values snapshot). */
export const INPUT_TYPES = new Set([
  'slider', 'switch', 'chips', 'textField', 'colorPicker',
  /* Feature Kit (v1.1 proposal) */
  'toggleRow', 'sliderRow', 'segmentRow', 'chipSelect', 'dial', 'timeWheel', 'colorSwatch',
]);

/** Render limits (nyaui.md §2): exceeding any of them rejects the whole tree. */
export const LIMITS = { maxDepth: 12, maxNodes: 200, maxString: 4096 } as const;
