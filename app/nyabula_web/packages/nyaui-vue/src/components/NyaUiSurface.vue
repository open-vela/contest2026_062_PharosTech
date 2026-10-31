<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import NyaUiPage from './NyaUiPage.vue';
import { validateTree } from '../validate';
import type { NyaUiTree, NyaUiCommand } from '../types';
type View = 'component' | 'html';
const props = withDefaults(defineProps<{ tree: NyaUiTree | null; pluginId?: string; busy?: boolean; view?: View; showSwitcher?: boolean }>(), { showSwitcher: true });
const emit = defineEmits<{ (e: 'command', value: NyaUiCommand): void; (e: 'update:view', value: View): void }>();
const local = ref<View>('component');
const available = computed<View[]>(() => props.tree?.views
  ? (['component', 'html'] as View[]).filter(v => !!props.tree?.views?.[v])
  : props.tree ? [typeof props.tree.html === 'string' ? 'html' : 'component'] : []);
const selected = computed(() => available.value.includes(props.view ?? local.value) ? (props.view ?? local.value) : available.value[0] ?? 'component');
const document = computed(() => props.tree?.views ? props.tree.views[selected.value] ?? null : props.tree);
const error = computed(() => validateTree(props.tree));
function choose(view: View) { local.value = view; emit('update:view', view); }
function command(event: NyaUiCommand) { emit('command', props.tree?.views ? { ...event, view: selected.value } : event); }
watch(() => props.pluginId, () => { local.value = 'component'; });
</script>
<template>
  <section class="nya-surface">
    <div v-if="showSwitcher && available.length > 1" class="view-switch" role="tablist" aria-label="插件显示方式">
      <button v-for="kind in available" :key="kind" role="tab" :aria-selected="selected === kind" :class="{selected:selected === kind}" @click="choose(kind)">{{ kind === 'component' ? '组件 UI' : 'HTML UI' }}</button>
    </div>
    <p v-if="error" class="surface-error" role="alert">{{ error }}</p>
    <NyaUiPage v-else :tree="document" :plugin-id="pluginId" :busy="busy" @command="command" />
  </section>
</template>
<style scoped>
.nya-surface { min-width:0; }
.view-switch { display:flex; gap:4px; margin-bottom:16px; padding:4px; width:fit-content; border:1px solid var(--md-outline-variant); border-radius:12px; }
.view-switch button { font:600 13px var(--font-body,sans-serif); padding:8px 14px; border:0; background:transparent; color:var(--md-on-surface-variant); border-radius:8px; cursor:pointer; }
.view-switch .selected { color:var(--md-on-primary-container); background:var(--md-primary-container); }
.surface-error { color:var(--md-error); }
</style>
