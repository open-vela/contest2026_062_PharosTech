<script setup lang="ts">
/* Permission overview, permission-first: which plugins request / hold each
 * permission. Desktop = matrix (rows = permissions, cols = plugins);
 * tablet / phone = per-permission groups expanding to plugin switches.
 * Owner toggles via plugins.setPermission; others see disabled switches. */
import { computed, inject, ref } from 'vue';
import { EmptyState, MdSwitch, Skeleton, permissionIcon, permissionZh } from '@nyabula/ui';
import { usePluginsStore, type PluginInfo } from '../../../stores/plugins';
import { useSessionStore } from '../../../stores/session';
import type { useFormFactor } from '../../../composables/useFormFactor';

const session = useSessionStore();
const plugins = usePluginsStore();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const matrix = computed(() => ff.formFactor.value === 'desktop');

interface PermRow {
  name: string;
  /** Plugins that declare this permission, with the current grant. */
  holders: { plugin: PluginInfo; granted: boolean }[];
  grantedCount: number;
}

const rows = computed<PermRow[]>(() => {
  const map = new Map<string, PermRow>();
  for (const p of plugins.list) {
    for (const perm of p.permissions ?? []) {
      let row = map.get(perm.name);
      if (!row) map.set(perm.name, (row = { name: perm.name, holders: [], grantedCount: 0 }));
      row.holders.push({ plugin: p, granted: perm.granted });
      if (perm.granted) row.grantedCount++;
    }
  }
  return [...map.values()].sort((a, b) => a.name.localeCompare(b.name));
});
const cols = computed(() => plugins.list.filter((p) => (p.permissions?.length ?? 0) > 0));

function cell(row: PermRow, plugin: PluginInfo): { granted: boolean } | null {
  return row.holders.find((h) => h.plugin.id === plugin.id) ?? null;
}
function toggle(pluginId: string, permission: string, granted: boolean): void {
  if (!session.isOwner) return;
  void plugins.setPermission(pluginId, permission, granted);
}

const open = ref<Record<string, boolean>>({});
function flip(name: string): void {
  open.value[name] = !open.value[name];
}
</script>

<template>
  <div class="perms">
    <p class="muted intro">
      按权限查看哪些插件申请并获得了授权。
      <span v-if="!session.isOwner" class="tag warn">仅主人可修改</span>
    </p>

    <Skeleton v-if="plugins.loading && !plugins.list.length" :lines="5" />
    <EmptyState v-else-if="plugins.error && !plugins.list.length" tone="error" title="插件列表读取失败" :hint="plugins.error" action-text="重试" @action="plugins.refresh()" />
    <EmptyState v-else-if="!rows.length" icon="shield" title="暂无权限申请" hint="已安装的插件都没有声明权限" />

    <div v-else-if="matrix" class="matrix-wrap">
      <table class="matrix">
        <thead>
          <tr>
            <th class="perm-h">权限</th>
            <th v-for="p in cols" :key="p.id" class="plug-h" :title="p.id">
              <span class="plug-name">{{ p.name }}</span>
              <span class="plug-ver mono">{{ p.version ?? '' }}</span>
            </th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="r in rows" :key="r.name">
            <th class="perm-cell">
              <span class="perm-icon"><svg viewBox="0 0 24 24" aria-hidden="true"><path :d="permissionIcon(r.name)" fill="currentColor" /></svg></span>
              <span class="perm-text">
                <span class="perm-zh">{{ permissionZh(r.name) }}</span>
                <span class="perm-id mono">{{ r.name }} · {{ r.grantedCount }}/{{ r.holders.length }}</span>
              </span>
            </th>
            <td v-for="p in cols" :key="p.id" class="cell">
              <MdSwitch
                v-if="cell(r, p)"
                :model-value="cell(r, p)!.granted"
                :disabled="!session.isOwner"
                @update:model-value="(v: boolean) => toggle(p.id, r.name, v)"
              />
              <span v-else class="dash">—</span>
            </td>
          </tr>
        </tbody>
      </table>
    </div>

    <div v-else class="stack">
      <div v-for="r in rows" :key="r.name" class="group" :class="{ open: open[r.name] }">
        <button class="group-head" @click="flip(r.name)">
          <span class="perm-icon"><svg viewBox="0 0 24 24" aria-hidden="true"><path :d="permissionIcon(r.name)" fill="currentColor" /></svg></span>
          <span class="perm-text">
            <span class="perm-zh">{{ permissionZh(r.name) }}</span>
            <span class="perm-id mono">{{ r.name }}</span>
          </span>
          <span class="tag" :class="r.grantedCount ? 'ok' : 'info'">{{ r.grantedCount }}/{{ r.holders.length }} 已授权</span>
          <span class="chev" :class="{ open: open[r.name] }">
            <svg viewBox="0 0 24 24" width="20" height="20" aria-hidden="true"><path d="M7.41 8.59L12 13.17l4.59-4.58L18 10l-6 6-6-6 1.41-1.41z" fill="currentColor" /></svg>
          </span>
        </button>
        <div v-if="open[r.name]" class="group-body">
          <div v-for="h in r.holders" :key="h.plugin.id" class="holder">
            <span class="holder-body">
              <span class="holder-name">{{ h.plugin.name }}</span>
              <span class="holder-id mono">{{ h.plugin.id }}</span>
            </span>
            <MdSwitch :model-value="h.granted" :disabled="!session.isOwner" @update:model-value="(v: boolean) => toggle(h.plugin.id, r.name, v)" />
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.intro { font-size: 13px; margin: 0 0 14px; display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.matrix-wrap { overflow-x: auto; border-radius: var(--radius-l); background: var(--md-surface-container); box-shadow: var(--md-elev-1); }
.matrix { border-collapse: collapse; min-width: 100%; }
.matrix th, .matrix td { padding: 10px 14px; border-bottom: 1px solid var(--md-outline-variant); text-align: left; }
.matrix tbody tr:last-child > * { border-bottom: none; }
.perm-h { font: 600 12px var(--font-body); letter-spacing: 0.08em; text-transform: uppercase; color: var(--md-on-surface-variant); }
.plug-h { text-align: center; vertical-align: bottom; min-width: 96px; }
.plug-name { display: block; font: 600 13px var(--font-body); color: var(--md-on-surface); }
.plug-ver { display: block; font-size: 11px; color: var(--md-on-surface-variant); }
.perm-cell { display: flex; align-items: center; gap: 10px; font-weight: normal; white-space: nowrap; }
.cell { text-align: center; }
.dash { color: var(--md-outline); }
.perm-icon {
  width: 34px;
  height: 34px;
  border-radius: var(--radius-m);
  display: grid;
  place-items: center;
  background: rgba(var(--md-primary-rgb), 0.12);
  color: var(--md-primary);
  flex: none;
}
.perm-icon svg { width: 18px; height: 18px; }
.perm-text { display: flex; flex-direction: column; min-width: 0; }
.perm-zh { font: 600 14px var(--font-body); color: var(--md-on-surface); }
.perm-id { font-size: 11.5px; color: var(--md-on-surface-variant); }
.group { background: var(--md-surface-container); border-radius: var(--radius-l); overflow: hidden; }
.group-head {
  width: 100%;
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px 14px;
  border: none;
  background: transparent;
  color: inherit;
  font: inherit;
  text-align: left;
  cursor: pointer;
}
.group-head .perm-text { flex: 1; }
.chev { color: var(--md-on-surface-variant); display: inline-flex; transition: transform var(--dur-fast); }
.chev.open { transform: rotate(180deg); }
.group-body { border-top: 1px solid var(--md-outline-variant); padding: 4px 14px 8px 60px; }
.holder { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 8px 0; }
.holder + .holder { border-top: 1px solid var(--md-outline-variant); }
.holder-body { display: flex; flex-direction: column; min-width: 0; }
.holder-name { font: 600 14px var(--font-body); color: var(--md-on-surface); }
.holder-id { font-size: 11.5px; color: var(--md-on-surface-variant); }
</style>
