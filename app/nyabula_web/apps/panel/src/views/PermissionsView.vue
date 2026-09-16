<script setup lang="ts">
/* Android-style permission manager backed by plugin.grant / plugin.revoke.
 * Two segments: "by plugin" (per-plugin permission list) and "by permission"
 * (permission groups listing every plugin that uses them). Only the owner
 * role may toggle; others see the switches disabled. */
import { computed, onMounted, ref } from 'vue';
import { MdCard, permissionZh, permissionIcon } from '@nyabula/ui';
import { useLinkStore } from '../stores/link';
import type { PluginEntry } from '../types/plugins';

const link = useLinkStore();
const plugins = ref<PluginEntry[]>([]);
const loading = ref(true);
const error = ref<string | null>(null);
const pendingKey = ref<string | null>(null);
const view = ref<'plugin' | 'permission'>('plugin');

const isOwner = computed(() => link.role === 'owner');

/** Permission-centric grouping: id -> plugins that declare it. */
const byPermission = computed(() => {
  const groups = new Map<string, { plugin: PluginEntry; granted: boolean }[]>();
  for (const p of plugins.value) {
    for (const perm of p.permissions ?? []) {
      const list = groups.get(perm.name) ?? [];
      list.push({ plugin: p, granted: perm.granted });
      groups.set(perm.name, list);
    }
  }
  return [...groups.entries()].sort((a, b) => a[0].localeCompare(b[0]));
});

async function refresh(): Promise<void> {
  loading.value = true;
  error.value = null;
  try {
    const res = await link.request('plugin.list', {});
    plugins.value = (res.plugins ?? []) as PluginEntry[];
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e);
  } finally {
    loading.value = false;
  }
}

async function toggle(plugin: PluginEntry, permName: string, granted: boolean): Promise<void> {
  if (!isOwner.value) return;
  const key = plugin.id + '/' + permName;
  pendingKey.value = key;
  error.value = null;
  const perm = plugin.permissions?.find((x) => x.name === permName);
  if (perm) perm.granted = !granted; // optimistic
  try {
    await link.request(granted ? 'plugin.revoke' : 'plugin.grant', {
      pluginId: plugin.id,
      permission: permName,
    });
  } catch (e) {
    if (perm) perm.granted = granted; // roll back
    error.value = e instanceof Error ? e.message : String(e);
  } finally {
    pendingKey.value = null;
  }
}

onMounted(() => void refresh());
</script>

<template>
  <div class="perms-view">
    <h2 class="title">权限</h2>
    <p v-if="!isOwner" class="hint">仅 owner 可以修改插件权限，当前角色：{{ link.role ?? '未知' }}。</p>
    <p v-if="error" class="err">{{ error }}</p>

    <!-- Segmented control: by plugin / by permission -->
    <div class="seg" role="tablist">
      <button
        type="button"
        role="tab"
        class="seg-btn"
        :class="{ active: view === 'plugin' }"
        :aria-selected="view === 'plugin'"
        @click="view = 'plugin'"
      >
        按插件
      </button>
      <button
        type="button"
        role="tab"
        class="seg-btn"
        :class="{ active: view === 'permission' }"
        :aria-selected="view === 'permission'"
        @click="view = 'permission'"
      >
        按权限
      </button>
    </div>

    <p v-if="loading" class="muted">加载中…</p>
    <p v-else-if="plugins.length === 0" class="muted">设备上还没有插件。</p>

    <!-- By plugin: one card per plugin, list-tiles of its permissions. -->
    <div v-else-if="view === 'plugin'" class="cards">
      <MdCard v-for="p in plugins" :key="p.id" :title="p.name">
        <p v-if="!p.permissions?.length" class="muted">该插件未声明权限。</p>
        <div v-for="perm in p.permissions" :key="perm.name" class="tile">
          <span class="tile-icon">
            <svg viewBox="0 0 24 24" aria-hidden="true"><path :d="permissionIcon(perm.name)" /></svg>
          </span>
          <span class="tile-text">
            <span class="tile-zh">{{ permissionZh(perm.name) }}</span>
            <span class="tile-id">{{ perm.name }}</span>
          </span>
          <button
            type="button"
            class="switch"
            :class="{ on: perm.granted }"
            role="switch"
            :aria-checked="perm.granted"
            :disabled="!isOwner || pendingKey === p.id + '/' + perm.name"
            @click="toggle(p, perm.name, perm.granted)"
          >
            <span class="thumb" />
          </button>
        </div>
      </MdCard>
    </div>

    <!-- By permission: one card per permission, list-tiles of plugins using it. -->
    <div v-else class="cards">
      <p v-if="byPermission.length === 0" class="muted">没有插件声明任何权限。</p>
      <MdCard v-for="[permName, users] in byPermission" :key="permName">
        <div class="group-head">
          <span class="tile-icon">
            <svg viewBox="0 0 24 24" aria-hidden="true"><path :d="permissionIcon(permName)" /></svg>
          </span>
          <span class="tile-text">
            <span class="tile-zh">{{ permissionZh(permName) }}</span>
            <span class="tile-id">{{ permName }}</span>
          </span>
        </div>
        <div v-for="u in users" :key="u.plugin.id" class="tile tile-plugin">
          <span class="tile-text">
            <span class="tile-zh">{{ u.plugin.name }}</span>
            <span class="tile-id">{{ u.plugin.id }}</span>
          </span>
          <button
            type="button"
            class="switch"
            :class="{ on: u.granted }"
            role="switch"
            :aria-checked="u.granted"
            :disabled="!isOwner || pendingKey === u.plugin.id + '/' + permName"
            @click="toggle(u.plugin, permName, u.granted)"
          >
            <span class="thumb" />
          </button>
        </div>
      </MdCard>
    </div>
  </div>
</template>

<style scoped>
.perms-view {
  max-width: 720px;
  margin: 0 auto;
  padding: 18px 20px 40px;
}
.title {
  font: 600 19px var(--font-title);
  color: var(--md-on-surface);
  margin-bottom: 14px;
}
.hint {
  background: var(--md-secondary-container);
  color: var(--md-on-secondary-container);
  border-radius: var(--radius-m);
  padding: 10px 14px;
  font-size: 13px;
  margin-bottom: 12px;
}
.err { color: var(--md-error); font-size: 13px; margin-bottom: 10px; }
.muted { color: var(--md-on-surface-variant); font-size: 14px; }
.seg {
  display: inline-flex;
  border: 1px solid var(--md-outline);
  border-radius: var(--radius-full);
  overflow: hidden;
  margin-bottom: 16px;
}
.seg-btn {
  background: transparent;
  border: none;
  color: var(--md-on-surface-variant);
  font: 600 13px var(--font-body);
  padding: 8px 20px;
  cursor: pointer;
}
.seg-btn.active {
  background: var(--md-secondary-container);
  color: var(--md-on-secondary-container);
}
.cards { display: flex; flex-direction: column; gap: 14px; }
.tile {
  display: flex;
  align-items: center;
  gap: 14px;
  padding: 9px 0;
}
.group-head {
  display: flex;
  align-items: center;
  gap: 14px;
  padding-bottom: 8px;
  margin-bottom: 4px;
  border-bottom: 1px solid var(--md-outline-variant);
}
.tile-plugin { padding-left: 54px; }
.tile-icon {
  width: 40px;
  height: 40px;
  flex: none;
  border-radius: 50%;
  background: var(--md-secondary-container);
  display: inline-flex;
  align-items: center;
  justify-content: center;
}
.tile-icon svg {
  width: 22px;
  height: 22px;
  fill: var(--md-on-secondary-container);
}
.tile-text {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.tile-zh {
  font: 500 14px var(--font-body);
  color: var(--md-on-surface);
}
.tile-id {
  font: 400 11.5px var(--font-body);
  color: var(--md-on-surface-variant);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.switch {
  width: 46px;
  height: 26px;
  border-radius: 13px;
  border: 1px solid var(--md-outline);
  background: var(--md-surface-container-highest);
  position: relative;
  cursor: pointer;
  transition: background 0.15s, border-color 0.15s;
  padding: 0;
  flex: none;
}
.switch:disabled { opacity: 0.45; cursor: default; }
.switch.on { background: var(--md-primary); border-color: var(--md-primary); }
.thumb {
  position: absolute;
  top: 3px;
  left: 3px;
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: var(--md-on-surface-variant);
  transition: left 0.15s, background 0.15s;
}
.switch.on .thumb { left: 23px; background: var(--md-on-primary); }
</style>
