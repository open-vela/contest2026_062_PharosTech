<script setup lang="ts">
import { MdCard, UiIcon } from '@nyabula/ui';
import type { useHomePage } from './home.logic';

const props = defineProps<{ page: ReturnType<typeof useHomePage> }>();
const { plugins, pluginCount, runningCount, go } = props.page;
</script>

<template>
  <MdCard title="插件">
    <div class="stats">
      <div class="stat"><span class="stat-val">{{ pluginCount }}</span><span class="stat-key">已安装</span></div>
      <div class="stat"><span class="stat-val">{{ runningCount }}</span><span class="stat-key">运行中</span></div>
    </div>
    <div v-if="plugins.list.length" class="chips">
      <button v-for="p in plugins.list.slice(0, 6)" :key="p.id" class="pchip" @click="go('plugin', { id: p.id })">
        <UiIcon :name="p.icon ?? 'extension'" :size="16" />{{ p.name }}
      </button>
    </div>
    <p v-else class="muted" style="font-size: 13px; margin: 0">尚未安装插件。</p>
    <button class="link" @click="go('plugins')">全部插件 <UiIcon name="chevron_right" :size="16" /></button>
  </MdCard>
</template>

<style scoped>
.stats { display: flex; gap: 18px; margin-bottom: 12px; }
.stat { display: flex; flex-direction: column; }
.stat-val { font: 600 26px var(--font-title); color: var(--md-on-surface); line-height: 1.1; }
.stat-key { font-size: 12px; color: var(--md-on-surface-variant); }
.chips { display: flex; flex-wrap: wrap; gap: 6px; margin-bottom: 10px; }
.pchip { display: inline-flex; align-items: center; gap: 5px; padding: 6px 10px; border-radius: 999px; border: 1px solid var(--md-outline-variant); background: transparent; color: var(--md-on-surface); font: 600 12.5px var(--font-body); cursor: pointer; }
.pchip:hover { background: var(--md-surface-container-high); }
.link { border: none; background: transparent; color: var(--md-primary); font: 600 13px var(--font-body); cursor: pointer; display: inline-flex; align-items: center; gap: 2px; padding: 0; }
</style>
