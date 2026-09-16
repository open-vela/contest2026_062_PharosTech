<script setup lang="ts">
/* One plugin widget: ui.tree {surface:"widget"} rendered with NyaUI. */
import { onMounted, ref } from 'vue';
import { useRouter } from 'vue-router';
import { NyaUiPage, type NyaUiCommand, type NyaUiTree } from '@nyabula/nyaui-vue';
import { MdCard, Skeleton, UiIcon } from '@nyabula/ui';
import { usePluginsStore, type PluginInfo } from '../stores/plugins';
import { useSessionStore } from '../stores/session';

const props = defineProps<{ plugin: PluginInfo }>();
const plugins = usePluginsStore();
const session = useSessionStore();
const router = useRouter();
const tree = ref<NyaUiTree | null>(null);
const failed = ref(false);
const busy = ref(false);

onMounted(async () => {
  try {
    tree.value = await plugins.loadTree(props.plugin.id, false, 'widget');
  } catch {
    failed.value = true;
  }
});
async function onCommand(cmd: NyaUiCommand) {
  busy.value = true;
  await plugins.sendEvent(props.plugin.id, cmd);
  busy.value = false;
}
function open() {
  if (session.deviceKey) void router.push({ name: 'plugin', params: { key: session.deviceKey, id: props.plugin.id } });
}
</script>

<template>
  <MdCard class="pw">
    <button class="pw-head" @click="open">
      <UiIcon :name="plugin.icon ?? 'extension'" :size="18" />
      <span>{{ plugin.name }}</span>
      <UiIcon name="chevron_right" :size="16" class="muted" />
    </button>
    <NyaUiPage v-if="tree" :tree="tree" :busy="busy" @command="onCommand" />
    <p v-else-if="failed" class="muted" style="font-size: 12.5px; margin: 0">此插件未提供小组件</p>
    <Skeleton v-else :lines="3" />
  </MdCard>
</template>

<style scoped>
.pw-head {
  display: flex;
  align-items: center;
  gap: 8px;
  border: none;
  background: transparent;
  color: var(--md-on-surface);
  font: 600 14px var(--font-body);
  padding: 0;
  margin-bottom: 10px;
  cursor: pointer;
  width: 100%;
}
.pw-head span { flex: 1; text-align: left; }
</style>
