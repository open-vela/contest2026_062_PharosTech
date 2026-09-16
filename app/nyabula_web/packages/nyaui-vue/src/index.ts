/* @nyabula/nyaui-vue — NyaUI DSL v1 renderer for Vue3.
 * Contract: Shared/nyaui/nyaui.md. */
export { default as NyaUiPage } from './components/NyaUiPage.vue';
export { default as NyaUiNode } from './components/NyaUiNode.vue';
export { default as NyaIcon } from './components/NyaIcon.vue';
export { default as NyaChart } from './components/NyaChart.vue';
export { default as NyaEye } from './components/NyaEye.vue';
export { defineNyaComponent, registerNyaComponent, getNyaComponent, listNyaComponents } from './registry.js';
export type { NyaComponentDef, NyaComponentKind } from './registry.js';
export { NYAUI_CTX } from './context.js';
export type { NyaUiContext } from './context.js';
export { applyPatches } from './patch.js';
export { validateTree } from './validate.js';
export { collectValues, resolveArgs } from './values.js';
export * from './types.js';

/* Feature Kit (v1.1 proposal): Nk* components as DSL types. Registered by
 * NyaUiPage on first mount; call registerKit() yourself when rendering
 * NyaUiNode without a page. */
export { registerKit, KIT_DEFS, KIT_TYPES } from './kit.js';

/* Advanced HTML plugin channel (v1.1 proposal). */
export { default as NyaHtmlFrame } from './components/NyaHtmlFrame.vue';
export { HTML_SDK_SOURCE, buildHtmlSdk } from './html-sdk.js';
export type {
  NyabulaHtmlSdk,
  NyaHtmlSendOptions,
  NyaHtmlEventMessage,
  NyaHtmlReadyMessage,
  NyaHtmlPatchMessage,
  NyaHtmlThemeMessage,
  NyaHtmlFrameToHost,
  NyaHtmlHostToFrame,
} from './html-sdk.js';
