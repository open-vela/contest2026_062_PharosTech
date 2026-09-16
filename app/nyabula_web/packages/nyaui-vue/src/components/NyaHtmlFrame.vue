<script setup lang="ts">
/* NyaHtmlFrame — advanced HTML plugin channel (nyaui.md v1.1 proposal).
 * Hosts plugin-authored HTML in a sandboxed iframe (`allow-scripts` only: no
 * same-origin, no navigation, no forms). The current `--md-*` theme tokens
 * are read from :root and injected as a <style> block plus the html-sdk
 * script, so the plugin can use the same design tokens as the host.
 *
 * Bridge (postMessage, envelope `nyaui: 1`):
 *   frame -> host  {type:'event', componentId, event, command, args, values}
 *                  -> emitted as `command` (same NyaUiCommand shape as NyaUiPage)
 *   host  -> frame {type:'patch', patches}  via the `patches` prop / `postPatches()`
 *   host  -> frame {type:'theme', vars}     automatically on theme change */
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { buildHtmlSdk, type NyaHtmlFrameToHost } from '../html-sdk.js';
import type { NyaUiCommand, NyaUiPatch } from '../types.js';

const props = withDefaults(
  defineProps<{
    html: string;
    pluginId: string;
    /** Patches to forward; every change of this array is posted to the frame. */
    patches?: NyaUiPatch[];
    /** Extra CSS var names (beyond --md-*, --radius-*, --dur*, --ease*, --font-*) to forward. */
    extraVars?: string[];
    height?: number | string;
    title?: string;
  }>(),
  { patches: () => [], extraVars: () => [], height: 320, title: 'plugin' },
);
const emit = defineEmits<{ (e: 'command', payload: NyaUiCommand): void; (e: 'ready'): void }>();

const frame = ref<HTMLIFrameElement>();
const ready = ref(false);
const themeVars = ref<Record<string, string>>({});

const VAR_PREFIXES = ['--md-', '--radius-', '--dur', '--ease', '--font-'];

/** Collect design-token custom properties currently applied on :root. */
function readThemeVars(): Record<string, string> {
  const out: Record<string, string> = {};
  if (typeof document === 'undefined') return out;
  const root = document.documentElement;
  const cs = getComputedStyle(root);
  const names = new Set<string>(props.extraVars);
  for (const sheet of Array.from(document.styleSheets)) {
    let rules: CSSRuleList;
    try {
      rules = sheet.cssRules;
    } catch {
      continue; // cross-origin sheet
    }
    for (const rule of Array.from(rules)) {
      if (!(rule instanceof CSSStyleRule)) continue;
      if (!/(^|,)\s*:root/.test(rule.selectorText)) continue;
      for (const name of Array.from(rule.style)) {
        if (VAR_PREFIXES.some((p) => name.startsWith(p))) names.add(name);
      }
    }
  }
  for (const name of names) {
    const v = cs.getPropertyValue(name).trim();
    if (v) out[name] = v;
  }
  return out;
}

/* A closing script/style tag inside injected JSON or CSS would end our tags early. */
function escapeInline(s: string): string {
  return s.replace(/<\//g, '<\\/');
}

const srcdoc = computed(() => {
  const vars = themeVars.value;
  const css = Object.entries(vars)
    .map(([k, v]) => `${k}:${v};`)
    .join('');
  const scheme = typeof document !== 'undefined' ? document.documentElement.dataset.theme === 'light' ? 'light' : 'dark' : 'dark';
  const head =
    `<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">` +
    `<style id="nya-theme">:root{${escapeInline(css)}color-scheme:${scheme};}` +
    `html,body{margin:0;background:transparent;color:var(--md-on-surface,inherit);font-family:var(--font-body,sans-serif);}</style>` +
    // Tag names are split so the SFC parser does not treat them as this block's end tag.
    `<${'script'}>${escapeInline(buildHtmlSdk(props.pluginId, vars))}</${'script'}>`;
  return `<!doctype html><html><head>${head}</head><body>${props.html}</body></html>`;
});

function post(msg: Record<string, unknown>): void {
  frame.value?.contentWindow?.postMessage({ nyaui: 1, ...msg }, '*');
}
/** Forward patches to the frame (also called automatically on prop change). */
function postPatches(patches: NyaUiPatch[]): void {
  if (patches.length) post({ type: 'patch', patches });
}
function postTheme(): void {
  post({ type: 'theme', vars: themeVars.value });
}

function onMessage(ev: MessageEvent): void {
  if (!frame.value || ev.source !== frame.value.contentWindow) return;
  const m = ev.data as NyaHtmlFrameToHost | undefined;
  if (!m || m.nyaui !== 1 || typeof m.type !== 'string') return;
  if (m.type === 'ready') {
    ready.value = true;
    emit('ready');
    postPatches(props.patches);
    return;
  }
  if (m.type === 'event' && typeof m.event === 'string') {
    emit('command', {
      componentId: typeof m.componentId === 'string' ? m.componentId : undefined,
      event: m.event,
      command: typeof m.command === 'string' ? m.command : undefined,
      args: m.args && typeof m.args === 'object' ? m.args : undefined,
      values: m.values && typeof m.values === 'object' ? m.values : {},
    });
  }
}

let themeObserver: MutationObserver | undefined;
onMounted(() => {
  themeVars.value = readThemeVars();
  window.addEventListener('message', onMessage);
  if (typeof MutationObserver !== 'undefined') {
    themeObserver = new MutationObserver(() => {
      themeVars.value = readThemeVars();
      postTheme();
    });
    themeObserver.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme', 'style'] });
  }
});
onBeforeUnmount(() => {
  window.removeEventListener('message', onMessage);
  themeObserver?.disconnect();
});
watch(
  () => props.patches,
  (p) => {
    if (ready.value) postPatches(p);
  },
);
/* A new html/pluginId re-renders srcdoc; the frame must announce ready again. */
watch(srcdoc, () => {
  ready.value = false;
});

defineExpose({ postPatches, postTheme, ready });
</script>

<template>
  <iframe
    ref="frame"
    class="nya-html-frame"
    :srcdoc="srcdoc"
    sandbox="allow-scripts"
    referrerpolicy="no-referrer"
    :title="title"
    :style="{ height: typeof height === 'number' ? height + 'px' : height }"
  />
</template>

<style scoped>
.nya-html-frame {
  display: block;
  width: 100%;
  border: none;
  border-radius: var(--radius-m, 14px);
  background: transparent;
  color-scheme: normal;
}
</style>
