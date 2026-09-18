/* Host extension registry: the seam for Core plugins to grow into the UI.
 * Contributions are plain objects; pages query by surface and form factor.
 *
 *   surface 'page'        plugin page (NyaUI tree; built-in)
 *   surface 'widget'      card on the eye / services pages (nyaui.md v1.1)
 *   surface 'quickAction' command palette / quick bar entries
 *   surface 'settings'    section under Device
 *   surface 'route'       fully custom Vue route under /d/:key/x/<id>
 */
import { reactive, type Component } from 'vue';
import type { FormFactor } from '../composables/useFormFactor';

export type Surface = 'page' | 'widget' | 'quickAction' | 'settings' | 'route';

export interface HostCommand {
  id: string;
  label: string;
  icon?: string;
  group?: string;
  run: () => void | Promise<unknown>;
}

export interface WidgetContribution {
  id: string;
  title: string;
  icon?: string;
  /** Vue component or a NyaUI tree provider. */
  component?: Component;
  pluginId?: string;
  /** Which form factors show it (default all). */
  formFactors?: FormFactor[];
  /** Grid size hint: 1 = quarter, 2 = half, 4 = full row. */
  span?: 1 | 2 | 4;
  order?: number;
}

export interface SettingsContribution {
  id: string;
  label: string;
  icon?: string;
  component: Component;
  order?: number;
}

export interface RouteContribution {
  id: string;
  label: string;
  icon?: string;
  component: Component;
}

const state = reactive({
  commands: [] as HostCommand[],
  widgets: [] as WidgetContribution[],
  settings: [] as SettingsContribution[],
  routes: [] as RouteContribution[],
});

function upsert<T extends { id: string }>(list: T[], item: T): () => void {
  const i = list.findIndex((x) => x.id === item.id);
  if (i >= 0) list.splice(i, 1, item);
  else list.push(item);
  return () => {
    const j = list.findIndex((x) => x.id === item.id);
    if (j >= 0) list.splice(j, 1);
  };
}

export const hostRegistry = {
  registerCommand: (c: HostCommand) => upsert(state.commands, c),
  registerWidget: (w: WidgetContribution) => upsert(state.widgets, w),
  registerSettings: (s: SettingsContribution) => upsert(state.settings, s),
  registerRoute: (r: RouteContribution) => upsert(state.routes, r),
  commands: () => state.commands,
  widgets: (ff?: FormFactor) =>
    [...state.widgets].filter((w) => !ff || !w.formFactors || w.formFactors.includes(ff)).sort((a, b) => (a.order ?? 0) - (b.order ?? 0)),
  settings: () => [...state.settings].sort((a, b) => (a.order ?? 0) - (b.order ?? 0)),
  routes: () => state.routes,
};

export type HostRegistry = typeof hostRegistry;
