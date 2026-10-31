<script setup lang="ts">
import PluginIcon from '../../components/PluginIcon.vue';
/* Tablet: landscape = master (list) / detail (NyaUI preview of the selected
 * plugin); portrait = plain card list. */
import { computed, inject } from 'vue';
import { EmptyState, MdButton, Skeleton, UiIcon } from '@nyabula/ui';
import PluginCard from './PluginCard.vue';
import PluginPagePane from './PluginPagePane.vue';
import PluginInstall from './PluginInstall.vue';
import PluginAuthorPanel from './PluginAuthorPanel.vue';
import { usePluginsPage } from './PluginsView.logic';
import { hasPage, permSummary, stateLabel, stateTone } from './plugins.logic';
import type { useFormFactor } from '../../composables/useFormFactor';

const page = usePluginsPage();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const portrait = computed(() => ff.orientation.value === 'portrait');
</script>

<template>
  <div class="plugins-tablet" :class="{ portrait }">
    <section class="master">
      <PluginInstall />
      <div class="row between head">
        <div>
          <h2 class="page-title">插件</h2>
          <p class="page-sub">{{ page.stats.value.total }} 个 · 运行中 {{ page.stats.value.running }} · 权限缺失 {{ page.stats.value.missing }}</p>
        </div>
        <MdButton variant="icon" :disabled="page.loading.value" aria-label="刷新" @click="page.refresh"><UiIcon name="refresh" :size="20" /></MdButton>
      </div>

      <PluginAuthorPanel />
      <div v-if="page.showSkeleton.value" class="stack">
        <Skeleton v-for="i in 5" :key="i" height="72px" radius="var(--radius-m)" />
      </div>
      <EmptyState v-else-if="page.showError.value" tone="error" title="插件列表加载失败" :hint="page.error.value ?? undefined" action-text="重试" @action="page.refresh" />
      <EmptyState v-else-if="page.showEmpty.value" icon="extension" title="还没有插件" hint="设备上尚未安装任何插件" action-text="刷新" @action="page.refresh" />
      <template v-else>
        <div v-if="portrait" class="grid-cards">
          <PluginCard v-for="p in page.items.value" :key="p.id" :plugin="p" @open="page.openPlugin(p.id)" />
        </div>
        <div v-else class="stack">
          <PluginCard v-for="p in page.items.value" :key="p.id" :plugin="p" :active="page.selectedId.value === p.id" @open="page.selectedId.value = p.id" />
        </div>
      </template>
    </section>

    <section v-if="!portrait" class="detail">
      <template v-if="page.selected.value">
        <div class="row between detail-head">
          <div class="row">
            <PluginIcon :plugin="page.selected.value" :size="22" />
            <span class="detail-title">{{ page.selected.value.name }}</span>
          </div>
          <MdButton variant="tonal" @click="page.openPlugin(page.selected.value.id)">打开</MdButton>
        </div>
        <PluginPagePane v-if="hasPage(page.selected.value)" :key="page.selected.value.id" :id="page.selected.value.id" />
        <EmptyState v-else compact icon="widgets" title="此插件没有页面" hint="它只提供小组件或后台能力" />
      </template>
      <EmptyState v-else compact icon="extension" title="选择一个插件预览" />
    </section>
  </div>
</template>

<style scoped>
.plugins-tablet {
  display: grid;
  grid-template-columns: minmax(300px, 1fr) minmax(0, 1.4fr);
  gap: 18px;
  padding: 18px 20px 40px;
  min-height: 100%;
}
.plugins-tablet.portrait { grid-template-columns: 1fr; }
.master { min-width: 0; display: flex; flex-direction: column; gap: 14px; }
.head { align-items: flex-start; }
.list-tile { width: 100%; text-align: left; border: 1px solid transparent; background: transparent; color: inherit; }
.list-tile.active { background: var(--md-secondary-container); color: var(--md-on-secondary-container); border-color: var(--md-primary); }
.detail {
  min-width: 0;
  border-radius: var(--radius-l, 18px);
  background: var(--md-surface-container-low);
  padding: 16px;
  display: flex;
  flex-direction: column;
  gap: 14px;
  align-self: start;
  position: sticky;
  top: 12px;
}
.detail-title { font: 600 16px var(--font-body); }
</style>
