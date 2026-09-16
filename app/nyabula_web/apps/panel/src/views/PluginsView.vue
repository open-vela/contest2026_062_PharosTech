<script setup lang="ts">
/* Plugin wall: plugin.list entries (icon + name + state), click into the
 * plugin's NyaUI sub-page. */
import { onMounted, ref } from 'vue';
import { NyaIcon } from '@nyabula/nyaui-vue';
import { useLinkStore } from '../stores/link';
import type { PluginEntry } from '../types/plugins';
import PluginPage from './PluginPage.vue';

const link = useLinkStore();
const plugins = ref<PluginEntry[]>([]);
const loading = ref(true);
const error = ref<string | null>(null);
const selected = ref<PluginEntry | null>(null);

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

onMounted(() => void refresh());
</script>

<template>
  <PluginPage
    v-if="selected"
    :plugin-id="selected.id"
    :plugin-name="selected.name"
    @back="selected = null"
  />
  <div v-else class="plugins-view">
    <div class="head">
      <h2 class="title">插件</h2>
      <button class="refresh" title="刷新" @click="refresh">⟳</button>
    </div>
    <p v-if="error" class="err">{{ error }}</p>
    <p v-else-if="loading" class="muted">加载中…</p>
    <p v-else-if="plugins.length === 0" class="muted">设备上还没有插件。</p>
    <div class="wall">
      <button v-for="p in plugins" :key="p.id" class="entry" @click="selected = p">
        <span class="entry-icon"><NyaIcon :name="p.icon" :size="26" /></span>
        <span class="entry-main">
          <span class="entry-name">{{ p.name }}</span>
          <span class="entry-meta">
            <span class="state" :class="'s-' + (p.state ?? 'unknown')">{{ p.state ?? '未知' }}</span>
            <span v-if="p.version" class="ver">v{{ p.version }}</span>
          </span>
        </span>
        <span class="chev">›</span>
      </button>
    </div>
  </div>
</template>

<style scoped>
.plugins-view {
  max-width: 720px;
  margin: 0 auto;
  padding: 18px 20px 40px;
}
.head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 14px;
}
.title {
  font: 600 19px var(--font-title);
  color: var(--md-on-surface);
}
.refresh {
  background: transparent;
  border: none;
  color: var(--md-on-surface-variant);
  font-size: 17px;
  cursor: pointer;
  padding: 6px;
  border-radius: 50%;
}
.refresh:hover {
  color: var(--md-on-surface);
  background: var(--md-surface-container-high);
}
.err { color: var(--md-error); font-size: 13px; }
.muted { color: var(--md-on-surface-variant); font-size: 14px; }
.wall {
  display: flex;
  flex-direction: column;
  gap: 10px;
}
.entry {
  display: flex;
  align-items: center;
  gap: 14px;
  text-align: left;
  background: var(--md-surface-container);
  border: none;
  border-radius: var(--radius-m);
  padding: 14px 16px;
  cursor: pointer;
  box-shadow: var(--md-elev-1);
  color: var(--md-on-surface);
}
.entry:hover { background: var(--md-surface-container-high); }
.entry-icon {
  width: 44px;
  height: 44px;
  border-radius: var(--radius-m);
  background: var(--md-secondary-container);
  color: var(--md-on-secondary-container);
  display: flex;
  align-items: center;
  justify-content: center;
  flex: none;
}
.entry-main { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 3px; }
.entry-name { font: 600 15px var(--font-body); }
.entry-meta { display: flex; gap: 8px; align-items: center; }
.state {
  font: 600 11px var(--font-body);
  padding: 1px 8px;
  border-radius: var(--radius-full);
  background: var(--md-surface-container-highest);
  color: var(--md-on-surface-variant);
}
.state.s-running { background: var(--md-primary-container); color: var(--md-on-primary-container); }
.state.s-error { background: rgba(255, 180, 171, 0.15); color: var(--md-error); }
.ver { font: 400 11px var(--font-body); color: var(--md-on-surface-variant); }
.chev { color: var(--md-on-surface-variant); font-size: 18px; }
</style>
