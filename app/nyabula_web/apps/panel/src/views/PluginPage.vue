<script setup lang="ts">
/* One plugin's NyaUI page: fetch ui.tree, render, forward commands as
 * ui.event, apply incoming ui.patch (refetch on patch failure). */
import { onBeforeUnmount, onMounted, ref } from 'vue';
import {
  NyaUiPage,
  applyPatches,
  type NyaUiCommand,
  type NyaUiPatch,
  type NyaUiTree,
} from '@nyabula/nyaui-vue';
import { useLinkStore } from '../stores/link';

const props = defineProps<{ pluginId: string; pluginName?: string }>();
const emit = defineEmits<{ (e: 'back'): void }>();

const link = useLinkStore();
const tree = ref<NyaUiTree | null>(null);
const loading = ref(true);
const busy = ref(false);
const error = ref<string | null>(null);
let unsub: (() => void) | null = null;

async function loadTree(): Promise<void> {
  loading.value = true;
  error.value = null;
  try {
    tree.value = (await link.request('ui.tree', { pluginId: props.pluginId })) as unknown as NyaUiTree;
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e);
  } finally {
    loading.value = false;
  }
}

async function onCommand(cmd: NyaUiCommand): Promise<void> {
  busy.value = true;
  error.value = null;
  try {
    const data: Record<string, unknown> = {
      pluginId: props.pluginId,
      componentId: cmd.componentId,
      event: cmd.event,
      values: cmd.values,
    };
    if (cmd.command !== undefined) data.command = cmd.command;
    if (cmd.args !== undefined) data.args = cmd.args;
    await link.request('ui.event', data);
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e);
  } finally {
    busy.value = false;
  }
}

onMounted(() => {
  void loadTree();
  unsub = link.onEvent('ui.patch', (data) => {
    if (data.pluginId !== props.pluginId || !tree.value) return;
    const patches = (data.patches ?? []) as NyaUiPatch[];
    if (!applyPatches(tree.value, patches)) {
      // Patch did not apply cleanly — server is authoritative, pull fresh.
      void loadTree();
    }
  });
});

onBeforeUnmount(() => {
  unsub?.();
  unsub = null;
});
</script>

<template>
  <div class="plugin-page">
    <div class="page-head">
      <button class="back-btn" @click="emit('back')">←</button>
      <h2 class="page-title">{{ tree?.page?.title ?? pluginName ?? pluginId }}</h2>
    </div>
    <p v-if="error" class="page-error">{{ error }}</p>
    <p v-if="loading" class="page-loading">加载中…</p>
    <NyaUiPage v-else :tree="tree" :busy="busy" @command="onCommand" />
  </div>
</template>

<style scoped>
.plugin-page {
  max-width: 720px;
  margin: 0 auto;
  padding: 18px 20px 40px;
}
.page-head {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 14px;
}
.back-btn {
  background: var(--md-surface-container-high);
  border: none;
  color: var(--md-on-surface);
  width: 36px;
  height: 36px;
  border-radius: 50%;
  font-size: 17px;
  cursor: pointer;
}
.back-btn:hover {
  background: var(--md-surface-container-highest);
}
.page-title {
  font: 600 19px var(--font-title);
  color: var(--md-on-surface);
}
.page-error {
  color: var(--md-error);
  font-size: 13px;
  margin-bottom: 10px;
}
.page-loading {
  color: var(--md-on-surface-variant);
  font-size: 14px;
}
</style>
