<script setup lang="ts">
import PluginIcon from '../../components/PluginIcon.vue';
/* Plugin summary card (desktop grid / tablet portrait). */
import { computed, ref } from 'vue';
import PermissionRow from './PermissionRow.vue';
import { UiIcon, MdSwitch, useToastStore } from '@nyabula/ui';
import { usePluginsStore, type PluginInfo } from '../../stores/plugins';
import { useSessionStore } from '../../stores/session';
import { permSummary, stateLabel, stateTone } from './plugins.logic';

const props = defineProps<{ plugin: PluginInfo; active?: boolean }>();
const emit = defineEmits<{ (e: 'open'): void }>();
const plugins = usePluginsStore();
const session = useSessionStore();
const toast = useToastStore();
const busy = ref(false);
const permissionsOpen = ref(false);
async function toggle(enabled: boolean) {
  busy.value = true;
  try { await plugins.manage(enabled ? 'start' : 'stop', props.plugin.id); }
  catch (error) { toast.error(error, '插件开关操作失败'); }
  finally { busy.value = false; }
}
const perms = computed(() => permSummary(props.plugin));
</script>

<template>
  <article class="ui-panel plugin-card" :class="{ active }">
    <div class="card-main">
    <button class="plugin-open" @click="emit('open')">
    <span class="icon"><PluginIcon :plugin="plugin" :size="26" /></span>
    <span class="body">
      <span class="name">{{ plugin.name || plugin.id }}</span>
      <span class="plugin-id mono" :title="plugin.id">{{ plugin.id }}</span>
      <span class="meta">
        <span v-if="plugin.version" class="mono">v{{ plugin.version }}</span>
        <span class="tag" :class="stateTone(plugin.state)">{{ stateLabel(plugin.state) }}</span>
      </span>
    </span>
    </button>
    <MdSwitch :model-value="plugin.state === 'running'" :disabled="busy || !session.isOwner || !session.connected" :aria-label="`${plugin.state === 'running' ? '停止' : '启动'}${plugin.name || plugin.id}`" @update:model-value="toggle" />
    </div>
    <button class="permission-toggle" :aria-expanded="permissionsOpen" :aria-controls="`plugin-permissions-${plugin.id}`" @click="permissionsOpen = !permissionsOpen">
      <UiIcon name="shield" :size="16" />
      <span :class="{ warn: perms.missing > 0 }">权限 · {{ perms.total ? `已授 ${perms.granted}/${perms.total}` : '无需权限' }}</span>
      <span class="permission-action">{{ permissionsOpen ? '收起' : '管理' }}</span>
      <UiIcon :name="permissionsOpen ? 'expand_less' : 'expand_more'" :size="18" />
    </button>
    <section v-if="permissionsOpen" :id="`plugin-permissions-${plugin.id}`" class="permission-editor" :aria-label="`${plugin.name || plugin.id}的权限`">
      <p v-if="!plugin.permissions?.length" class="permission-hint">此插件未申请权限。</p>
      <p v-else-if="!session.isOwner" class="permission-hint">仅主人可修改权限。</p>
      <PermissionRow v-for="permission in plugin.permissions" :key="permission.name"
        :name="permission.name" :granted="permission.granted" compact
        :disabled="!session.isOwner || !session.connected"
        :busy="plugins.permissionPending(plugin.id, permission.name)"
        @toggle="plugins.setPermission(plugin.id, permission.name, $event)" />
    </section>
  </article>
</template>

<style scoped>
.plugin-card {
  display: flex;
  flex-direction: column;
  align-self: start;
  gap: 14px;
  width: 100%;
  text-align: left;
  padding: 16px;
  transition: background var(--dur-short, 0.15s) var(--ease-standard, ease), transform var(--dur-short, 0.15s) var(--ease-standard, ease);
}
.card-main { display: flex; align-items: center; gap: 14px; width: 100%; }
.plugin-card:hover { background: var(--md-surface-container-high); }

.plugin-card.active { box-shadow: inset 0 0 0 1px var(--md-primary); }
.icon {
  width: 48px;
  height: 48px;
  flex: none;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border-radius: var(--radius-m);
  background: var(--md-primary-container);
  color: var(--md-on-primary-container);
}
.body { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 6px; }
.name { font: 600 16px var(--font-body); overflow-wrap: anywhere; }
.plugin-id { font-size: 11px; color: var(--md-on-surface-variant); overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
.plugin-open { min-width: 0; flex: 1; display: flex; align-items: center; gap: 14px; text-align: left; border: 0; padding: 0; background: transparent; color: inherit; cursor: pointer; }
.meta { display: flex; align-items: center; gap: 8px; font-size: 12px; color: var(--md-on-surface-variant); }
.perm { display: inline-flex; align-items: center; gap: 5px; font-size: 12.5px; color: var(--md-on-surface-variant); }
.perm.warn { color: var(--md-warning); }
.chev { color: var(--md-on-surface-variant); margin-top: 14px; }
.permission-toggle { display: flex; align-items: center; gap: 6px; width: 100%; min-height: 36px; padding: 6px 0 0; border: 0; border-top: 1px solid var(--md-outline-variant); background: transparent; color: var(--md-on-surface-variant); font: 500 13px var(--font-body); text-align: left; cursor: pointer; }
.permission-toggle .warn { color: var(--md-warning); }
.permission-action { margin-left: auto; color: var(--md-primary); }
.permission-editor { display: flex; flex-direction: column; gap: 8px; width: 100%; min-width: 0; }
.permission-hint { margin: 0; color: var(--md-on-surface-variant); font-size: 13px; }
.permission-toggle:focus-visible, .plugin-open:focus-visible { outline: 2px solid var(--md-primary); outline-offset: 4px; border-radius: 4px; }
</style>
