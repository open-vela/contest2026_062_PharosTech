/* Plugin list page logic shared by the three variants. */
import { computed, ref, watch } from 'vue';
import { useAsyncTask } from '../../composables/useRequest';
import { usePluginsStore, type PluginInfo } from '../../stores/plugins';
import { useSessionStore } from '../../stores/session';
import { isRunning, permSummary, usePluginNav } from './plugins.logic';

export function usePluginsPage() {
  const session = useSessionStore();
  const plugins = usePluginsStore();
  const nav = usePluginNav();

  const task = useAsyncTask(() => plugins.refresh(), { holdRoute: true, immediate: true });
  // Re-fetch when a (re)connection lands and the list is empty.
  watch(
    () => session.connected,
    (c) => {
      if (c && plugins.list.length === 0) void task.run();
    },
  );

  const items = computed<PluginInfo[]>(() => plugins.list);
  const loading = computed(() => plugins.loading || task.busy.value);
  const error = computed(() => plugins.error);
  const showSkeleton = computed(() => loading.value && items.value.length === 0);
  const showError = computed(() => !!error.value && items.value.length === 0 && !loading.value);
  const showEmpty = computed(() => !error.value && !loading.value && items.value.length === 0);

  const stats = computed(() => ({
    total: items.value.length,
    running: items.value.filter((p) => isRunning(p.state)).length,
    missing: items.value.filter((p) => permSummary(p).missing > 0).length,
  }));

  /** Tablet landscape master-detail selection; defaults to the first plugin. */
  const selectedId = ref<string | null>(null);
  watch(
    items,
    (list) => {
      if (!list.some((p) => p.id === selectedId.value)) selectedId.value = list[0]?.id ?? null;
    },
    { immediate: true },
  );
  const selected = computed(() => items.value.find((p) => p.id === selectedId.value) ?? null);

  async function refresh() {
    await task.run();
  }

  return { session, plugins, items, loading, error, showSkeleton, showError, showEmpty, stats, selectedId, selected, refresh, ...nav };
}
