/* Shared helpers for the plugin domain (list / page / permissions). */
import { computed, ref, watch, type Ref } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { describeError } from '@nyabula/ui';
import type { NyaUiCommand } from '@nyabula/nyaui-vue';
import { useAsyncTask } from '../../composables/useRequest';
import { usePluginsStore, type PluginInfo } from '../../stores/plugins';
import { useSessionStore } from '../../stores/session';

export type Tone = 'ok' | 'warn' | 'err' | 'info';

/** Plugin state -> tag tone. The contract only says `state` is a string,
 * so common vocabularies are mapped and anything else falls back to info. */
export function stateTone(state?: string): Tone {
  const s = (state ?? '').toLowerCase();
  if (['running', 'active', 'enabled', 'ready', 'ok'].includes(s)) return 'ok';
  if (['error', 'crashed', 'failed', 'fault'].includes(s)) return 'err';
  if (['loading', 'starting', 'installing', 'updating', 'pending'].includes(s)) return 'warn';
  return 'info';
}

export function stateLabel(state?: string): string {
  const s = (state ?? '').toLowerCase();
  const map: Record<string, string> = {
    running: '运行中',
    active: '运行中',
    enabled: '已启用',
    ready: '就绪',
    ok: '正常',
    stopped: '已停止',
    disabled: '已禁用',
    idle: '空闲',
    error: '异常',
    crashed: '已崩溃',
    failed: '失败',
    fault: '故障',
    loading: '加载中',
    starting: '启动中',
    installing: '安装中',
    updating: '更新中',
    pending: '等待中',
  };
  return map[s] ?? (state ? state : '未知');
}

export function isRunning(state?: string): boolean {
  return stateTone(state) === 'ok';
}

export function permSummary(p: PluginInfo): { granted: number; total: number; missing: number } {
  const perms = p.permissions ?? [];
  const granted = perms.filter((x) => x.granted).length;
  return { granted, total: perms.length, missing: perms.length - granted };
}

export function hasPage(p: PluginInfo): boolean {
  return !p.ui || p.ui.length === 0 || p.ui.includes('page');
}

/** Device key for route params: route param first, then the live session. */
export function useDeviceKey(explicit?: Ref<string | undefined>) {
  const route = useRoute();
  const session = useSessionStore();
  return computed(() => explicit?.value ?? (route.params.key as string | undefined) ?? session.deviceKey ?? '');
}

/** Navigation shortcuts shared by the three pages. */
export function usePluginNav(explicitKey?: Ref<string | undefined>) {
  const router = useRouter();
  const key = useDeviceKey(explicitKey);
  function openPlugin(id: string) {
    void router.push({ name: 'plugin', params: { key: key.value, id } });
  }
  function openPermissions(id: string) {
    void router.push({ name: 'plugin-permissions', params: { key: key.value, id } });
  }
  function openList() {
    void router.push({ name: 'plugins', params: { key: key.value } });
  }
  return { key, openPlugin, openPermissions, openList };
}

/** Plugin page state: reactive tree from the store (ui.patch already applied
 * there; `treeVersion` is read so patches re-render), command forwarding,
 * load / retry with error surface. */
export function usePluginPage(id: Ref<string>, opts: { holdRoute?: boolean; surface?: string } = {}) {
  const plugins = usePluginsStore();
  const session = useSessionStore();
  const busy = ref(false);

  const treeKey = computed(() => (opts.surface ? `${id.value}#${opts.surface}` : id.value));
  const tree = computed(() => {
    void plugins.treeVersion; // ui.patch bumps this; mutations inside the tree are reactive too
    return plugins.trees[treeKey.value] ?? null;
  });
  const plugin = computed(() => plugins.list.find((p) => p.id === id.value) ?? null);
  const title = computed(() => tree.value?.page?.title ?? plugin.value?.name ?? id.value);

  let force = false;
  const task = useAsyncTask(
    async () => {
      if (plugins.list.length === 0 && !plugins.loading) await plugins.refresh();
      const f = force;
      force = false;
      return plugins.loadTree(id.value, f, opts.surface);
    },
    { holdRoute: opts.holdRoute ?? false, immediate: true },
  );
  watch(id, () => void task.run());

  const errorText = computed(() => (task.error.value ? describeError(task.error.value) : null));

  /** Retry = forced reload (bypasses the tree cache). */
  async function retry() {
    force = true;
    await task.run();
  }

  async function onCommand(cmd: NyaUiCommand) {
    if (!session.canControl) return;
    busy.value = true;
    try {
      await plugins.sendEvent(id.value, cmd);
    } finally {
      busy.value = false;
    }
  }

  const rawJson = computed(() => (tree.value ? JSON.stringify(tree.value, null, 2) : ''));

  return { plugins, session, tree, plugin, title, busy, loading: task.busy, errorText, retry, onCommand, rawJson };
}

/** Permission editing shared by page context panel / sheet / permissions view. */
export function usePluginPermissions(id: Ref<string>) {
  const plugins = usePluginsStore();
  const session = useSessionStore();
  const plugin = computed(() => plugins.list.find((p) => p.id === id.value) ?? null);
  const perms = computed(() => plugin.value?.permissions ?? []);
  const canEdit = computed(() => session.isOwner && session.connected);
  const pending = ref<Record<string, boolean>>({});
  const summary = computed(() => (plugin.value ? permSummary(plugin.value) : { granted: 0, total: 0, missing: 0 }));

  async function toggle(name: string, granted: boolean) {
    if (!canEdit.value || pending.value[name]) return;
    pending.value = { ...pending.value, [name]: true };
    try {
      await plugins.setPermission(id.value, name, granted);
    } finally {
      const next = { ...pending.value };
      delete next[name];
      pending.value = next;
    }
  }

  return { plugins, session, plugin, perms, canEdit, pending, summary, toggle };
}
