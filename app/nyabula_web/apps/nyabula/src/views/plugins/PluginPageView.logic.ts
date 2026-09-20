/* Plugin page (NyaUI) logic shared by the three variants. */
import { computed, ref, toRef, type Ref } from 'vue';
import { usePluginNav, usePluginPage, usePluginPermissions } from './plugins.logic';

export function usePluginPageView(props: { key?: string; id: string }) {
  const id = toRef(props, 'id') as Ref<string>;
  const nav = usePluginNav(toRef(props, 'key'));
  const page = usePluginPage(id, { holdRoute: true });
  const perms = usePluginPermissions(id);
  const devOpen = ref(false);
  const patchCount = computed(() => page.plugins.treeVersion);
  return { id, page, perms, devOpen, patchCount, ...nav };
}
