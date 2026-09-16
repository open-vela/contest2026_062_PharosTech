/* Plugin-side SDK for the advanced HTML channel (nyaui.md v1.1 proposal).
 *
 * `HTML_SDK_SOURCE` is a self-contained ES5 script string that NyaHtmlFrame
 * injects into the sandboxed iframe's srcdoc. Inside the frame it exposes:
 *
 *   window.nyabula.send(event, opts?)   -> post an event to the host
 *   window.nyabula.onPatch(cb)          -> subscribe to ui.patch forwards
 *   window.nyabula.onTheme(cb)          -> subscribe to theme var updates
 *   window.nyabula.theme                -> current {"--md-primary": "#..."} map
 *   window.nyabula.pluginId             -> owning plugin id
 *
 * Message envelope (both directions) always carries `nyaui: 1`.
 *   frame -> host  {nyaui:1, type:'event', componentId?, event, command?, args?, values?}
 *   frame -> host  {nyaui:1, type:'ready'}
 *   host -> frame  {nyaui:1, type:'patch', patches:[{path,value}]}
 *   host -> frame  {nyaui:1, type:'theme', vars:{...}}
 *
 * Keep this file free of imports so the string stays the single source. */

export interface NyaHtmlEventMessage {
  nyaui: 1;
  type: 'event';
  componentId?: string;
  event: string;
  command?: string;
  args?: Record<string, unknown>;
  values?: Record<string, unknown>;
}
export interface NyaHtmlReadyMessage {
  nyaui: 1;
  type: 'ready';
}
export interface NyaHtmlPatchMessage {
  nyaui: 1;
  type: 'patch';
  patches: { path: string; value: unknown }[];
}
export interface NyaHtmlThemeMessage {
  nyaui: 1;
  type: 'theme';
  vars: Record<string, string>;
}
export type NyaHtmlFrameToHost = NyaHtmlEventMessage | NyaHtmlReadyMessage;
export type NyaHtmlHostToFrame = NyaHtmlPatchMessage | NyaHtmlThemeMessage;

/** Options accepted by `window.nyabula.send(event, opts)` inside the frame. */
export interface NyaHtmlSendOptions {
  componentId?: string;
  command?: string;
  args?: Record<string, unknown>;
  values?: Record<string, unknown>;
}

/** Shape of `window.nyabula` inside the frame (for plugin authors' typings). */
export interface NyabulaHtmlSdk {
  pluginId: string;
  theme: Record<string, string>;
  send(event: string, opts?: NyaHtmlSendOptions): void;
  onPatch(cb: (patches: { path: string; value: unknown }[]) => void): () => void;
  onTheme(cb: (vars: Record<string, string>) => void): () => void;
}

/** Placeholder replaced by NyaHtmlFrame with the JSON-encoded plugin id. */
export const SDK_PLUGIN_ID_TOKEN = '__NYA_PLUGIN_ID__';
/** Placeholder replaced with the JSON-encoded initial theme var map. */
export const SDK_THEME_TOKEN = '__NYA_THEME__';

export const HTML_SDK_SOURCE = `(function () {
  var patchCbs = [];
  var themeCbs = [];
  var sdk = {
    pluginId: ${SDK_PLUGIN_ID_TOKEN},
    theme: ${SDK_THEME_TOKEN},
    send: function (event, opts) {
      opts = opts || {};
      window.parent.postMessage({
        nyaui: 1,
        type: 'event',
        componentId: opts.componentId,
        event: String(event),
        command: opts.command,
        args: opts.args,
        values: opts.values
      }, '*');
    },
    onPatch: function (cb) {
      patchCbs.push(cb);
      return function () { patchCbs = patchCbs.filter(function (f) { return f !== cb; }); };
    },
    onTheme: function (cb) {
      themeCbs.push(cb);
      return function () { themeCbs = themeCbs.filter(function (f) { return f !== cb; }); };
    }
  };
  function applyTheme(vars) {
    var root = document.documentElement;
    for (var k in vars) if (Object.prototype.hasOwnProperty.call(vars, k)) {
      sdk.theme[k] = vars[k];
      root.style.setProperty(k, vars[k]);
    }
    themeCbs.forEach(function (f) { try { f(vars); } catch (e) {} });
  }
  window.addEventListener('message', function (ev) {
    var m = ev.data;
    if (!m || m.nyaui !== 1) return;
    if (m.type === 'patch' && Array.isArray(m.patches)) {
      patchCbs.forEach(function (f) { try { f(m.patches); } catch (e) {} });
    } else if (m.type === 'theme' && m.vars && typeof m.vars === 'object') {
      applyTheme(m.vars);
    }
  });
  window.nyabula = sdk;
  window.parent.postMessage({ nyaui: 1, type: 'ready' }, '*');
})();`;

/** Render the SDK source with the plugin id and theme baked in. */
export function buildHtmlSdk(pluginId: string, theme: Record<string, string>): string {
  return HTML_SDK_SOURCE.replace(SDK_PLUGIN_ID_TOKEN, JSON.stringify(pluginId)).replace(
    SDK_THEME_TOKEN,
    JSON.stringify(theme),
  );
}
