/* Plugin domain: plugin.list cache, per-plugin ui.tree cache with ui.patch
 * application, grant/revoke. */
import { defineStore } from 'pinia';
import { ref, watch } from 'vue';
import { applyPatches, type NyaUiPatch, type NyaUiTree } from '@nyabula/nyaui-vue';
import { useToastStore } from '@nyabula/ui';
import { useSessionStore } from './session';

export interface PluginPermission {
  name: string;
  granted: boolean;
}
export interface PluginInfo {
  id: string;
  name: string;
  version?: string;
  state?: string;
  icon?: string;
  ui?: string[];
  permissions?: PluginPermission[];
  /** v1.1 proposal: host surfaces beyond "page". */
  surfaces?: string[];
  actions?: { id: string; label: string; icon?: string; command: string }[];
}

export const usePluginsStore = defineStore('plugins', () => {
  const session = useSessionStore();
  const toast = useToastStore();
  const list = ref<PluginInfo[]>([]);
  const loading = ref(false);
  const error = ref<string | null>(null);
  const trees = ref<Record<string, NyaUiTree>>({});
  const treeVersion = ref(0);

  let unsub: (() => void) | null = null;
  watch(
    () => session.client,
    (c) => {
      unsub?.();
      unsub = null;
      list.value = [];
      trees.value = {};
      if (!c) return;
      unsub = c.on('ui.patch', (data) => {
        const id = String(data.pluginId ?? '');
        const tree = trees.value[id];
        if (!tree) return;
        const patches = (data.patches ?? []) as NyaUiPatch[];
        if (!applyPatches(tree, patches)) void loadTree(id, true);
        else treeVersion.value++;
      });
    },
    { immediate: true },
  );

  async function refresh(): Promise<void> {
    loading.value = true;
    error.value = null;
    try {
      const r = await session.request('plugin.list');
      list.value = (r.plugins ?? []) as PluginInfo[];
    } catch (e) {
      error.value = e instanceof Error ? e.message : String(e);
    } finally {
      loading.value = false;
    }
  }

  async function loadTree(pluginId: string, force = false, surface?: string): Promise<NyaUiTree | null> {
    const key = surface ? `${pluginId}#${surface}` : pluginId;
    if (!force && trees.value[key]) return trees.value[key]!;
    const data: Record<string, unknown> = { pluginId };
    if (surface) data.surface = surface;
    const tree = (await session.request('ui.tree', data)) as unknown as NyaUiTree;
    trees.value[key] = tree;
    treeVersion.value++;
    return tree;
  }

  async function sendEvent(pluginId: string, cmd: { componentId?: string; event: string; command?: string; args?: unknown; values?: unknown }): Promise<boolean> {
    const data: Record<string, unknown> = { pluginId, componentId: cmd.componentId, event: cmd.event, values: cmd.values };
    if (cmd.command !== undefined) data.command = cmd.command;
    if (cmd.args !== undefined) data.args = cmd.args;
    try {
      await session.request('ui.event', data);
      return true;
    } catch (e) {
      toast.error(e, '插件操作失败');
      return false;
    }
  }

  async function setPermission(pluginId: string, permission: string, granted: boolean): Promise<boolean> {
    const p = list.value.find((x) => x.id === pluginId)?.permissions?.find((x) => x.name === permission);
    const prev = p?.granted;
    if (p) p.granted = granted;
    try {
      await session.request(granted ? 'plugin.grant' : 'plugin.revoke', { pluginId, permission });
      toast.ok(granted ? `已授予 ${permission}` : `已撤销 ${permission}`);
      return true;
    } catch (e) {
      if (p && prev !== undefined) p.granted = prev;
      toast.error(e, '权限变更失败');
      return false;
    }
  }

  return { list, loading, error, trees, treeVersion, refresh, loadTree, sendEvent, setPermission };
});
