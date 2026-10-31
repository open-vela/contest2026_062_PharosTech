/* Plugin domain: plugin.list cache, per-plugin ui.tree cache with ui.patch
 * application, grant/revoke. */
import { defineStore } from 'pinia';
import { computed, ref, shallowRef, watch } from 'vue';
import { applyPatches, type NyaUiPatch, type NyaUiTree } from '@nyabula/nyaui-vue';
import { useToastStore, permissionZh } from '@nyabula/ui';
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
  lastError?: string | number;
  versions?: { version: string; current: boolean; lastGood: boolean }[];
  icon?: string | { type: 'image' | 'iconfont'; path: string; codepoint?: string };
  ui?: string[];
  permissions?: PluginPermission[];
  /** v1.1 proposal: host surfaces beyond "page". */
  surfaces?: string[];
  actions?: { id: string; label: string; icon?: string; command: string }[];
}

export const usePluginsStore = defineStore('plugins', () => {
  const session = useSessionStore();
  const toast = useToastStore();
  const confirmedList = ref<PluginInfo[]>([]);
  interface PermissionChange {
    pluginId: string;
    permission: string;
    granted: boolean;
    client: typeof session.client;
    resolve: (success: boolean) => void;
  }
  const permissionQueue = shallowRef<PermissionChange[]>([]);
  let permissionRunner: object | null = null;
  let listRequest = 0;
  let permissionRevision = 0;
  // Keep requested switch positions visible while queued writes or stale
  // ui.changed refreshes are in flight. Only acknowledgements update truth.
  const list = computed<PluginInfo[]>(() => confirmedList.value.map(plugin => ({
    ...plugin,
    permissions: plugin.permissions?.map(permission => {
      const changes = permissionQueue.value.filter(change =>
        change.pluginId === plugin.id && change.permission === permission.name);
      const requested = changes[changes.length - 1];
      return requested ? { ...permission, granted: requested.granted } : permission;
    }),
  })));
  const permissionQueueLength = computed(() => permissionQueue.value.length);
  function permissionPending(pluginId: string, permission: string): boolean {
    return permissionQueue.value.some(change => change.pluginId === pluginId && change.permission === permission);
  }
  const loading = ref(false);
  const error = ref<string | null>(null);
  const trees = ref<Record<string, NyaUiTree>>({});
  const treeVersion = ref(0);

  const requestedTrees = new Map<string, { pluginId: string; surface?: string }>();
  const treeRequests = new Map<string, number>();
  let requestSequence = 0;
  let unsub: (() => void) | null = null;
  watch(
    () => session.client,
    (c) => {
      unsub?.();
      unsub = null;
      permissionRunner = null;
      for (const change of permissionQueue.value) change.resolve(false);
      permissionQueue.value = [];
      permissionRevision++;
      listRequest++;
      loading.value = false;
      confirmedList.value = [];
      trees.value = {};
      requestedTrees.clear();
      treeRequests.clear();
      if (!c) return;
      const offPatch = c.on('ui.patch', (data) => {
        const id = String(data.pluginId ?? '');
        const tree = trees.value[id];
        if (!tree) return;
        const patches = (data.patches ?? []) as NyaUiPatch[];
        if (!applyPatches(tree, patches)) void loadTree(id, true);
        else treeVersion.value++;
      });
      const offChanged = c.on('ui.changed', () => {
        void refresh();
        for (const { pluginId, surface } of requestedTrees.values()) {
          void loadTree(pluginId, true, surface).catch(() => {});
        }
      });
      unsub = () => { offPatch(); offChanged(); };
    },
    { immediate: true },
  );

  async function refresh(): Promise<void> {
    const sequence = ++listRequest;
    const revision = permissionRevision;
    const client = session.client;
    loading.value = true;
    error.value = null;
    try {
      const r = await session.request('plugin.list');
      if (client !== session.client || sequence !== listRequest || revision !== permissionRevision) return;
      confirmedList.value = (r.plugins ?? []) as PluginInfo[];
    } catch (e) {
      if (client === session.client && sequence === listRequest && revision === permissionRevision)
        error.value = e instanceof Error ? e.message : String(e);
    } finally {
      if (client === session.client && sequence === listRequest) loading.value = false;
    }
  }

  async function loadTree(pluginId: string, force = false, surface?: string): Promise<NyaUiTree | null> {
    const key = surface ? `${pluginId}#${surface}` : pluginId;
    requestedTrees.set(key, { pluginId, surface });
    if (!force && trees.value[key]) return trees.value[key]!;
    const data: Record<string, unknown> = { pluginId };
    if (surface) data.surface = surface;
    const sequence = ++requestSequence;
    treeRequests.set(key, sequence);
    const client = session.client;
    try {
      const tree = (await session.request('ui.tree', data)) as unknown as NyaUiTree;
      if (client !== session.client || treeRequests.get(key) !== sequence) return trees.value[key] ?? null;
      trees.value[key] = tree;
      treeVersion.value++;
      return tree;
    } catch (e) {
      if (client === session.client && treeRequests.get(key) === sequence) {
        delete trees.value[key];
        treeVersion.value++;
      }
      throw e;
    }
  }

  async function sendEvent(pluginId: string, cmd: { view?: 'component' | 'html'; componentId?: string; event: string; command?: string; args?: unknown; values?: unknown }): Promise<boolean> {
    const data: Record<string, unknown> = { pluginId, view: cmd.view, componentId: cmd.componentId, event: cmd.event, values: cmd.values };
    if (cmd.command !== undefined) data.command = cmd.command;
    if (cmd.args !== undefined) data.args = cmd.args;
    try {
      await session.request('ui.event', data);
      await loadTree(pluginId, true).catch(() => {});
      return true;
    } catch (e) {
      toast.error(e, '插件操作失败');
      return false;
    }
  }

  async function drainPermissions(token: object): Promise<void> {
    while (permissionRunner === token && permissionQueue.value.length) {
      const change = permissionQueue.value[0];
      let success = false;
      try {
        if (change.client !== session.client) return;
        const r = await session.request(change.granted ? 'plugin.grant' : 'plugin.revoke', {
          pluginId: change.pluginId, permission: change.permission,
        });
        if (permissionRunner !== token || change.client !== session.client) return;
        permissionRevision++;
        if (Array.isArray(r.plugins)) confirmedList.value = r.plugins as PluginInfo[];
        else {
          // Simulator and earlier endpoints acknowledge a single plugin.
          const permission = confirmedList.value.find(p => p.id === change.pluginId)
            ?.permissions?.find(p => p.name === change.permission);
          if (permission) permission.granted = change.granted;
        }
        toast.ok(change.granted ? `已授予 ${permissionZh(change.permission)}` : `已撤销 ${permissionZh(change.permission)}`);
        success = true;
      } catch (e) {
        if (permissionRunner === token && change.client === session.client)
          toast.error(e, '权限变更失败');
      } finally {
        if (permissionRunner === token) {
          permissionQueue.value = permissionQueue.value.slice(1);
          change.resolve(success);
        }
      }
    }
    if (permissionRunner === token) permissionRunner = null;
  }

  function setPermission(pluginId: string, permission: string, granted: boolean): Promise<boolean> {
    return new Promise(resolve => {
      permissionQueue.value = [...permissionQueue.value, { pluginId, permission, granted, client: session.client, resolve }];
      if (!permissionRunner) {
        permissionRunner = {};
        void drainPermissions(permissionRunner);
      }
    });
  }

  async function manage(action: 'start' | 'stop' | 'activate' | 'rollback', pluginId: string, version?: string): Promise<void> {
    await session.request(`plugin.${action}`, { pluginId, version });
    delete trees.value[pluginId];
    await refresh();
  }

  async function install(path: string): Promise<void> {
    await session.request('plugin.install', { path });
    await refresh();
  }

  return { manage, install, list, permissionQueueLength, permissionPending, loading, error, trees, treeVersion, refresh, loadTree, sendEvent, setPermission };
});
