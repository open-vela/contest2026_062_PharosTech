<script setup lang="ts">
/* Desktop: card grid; stats + refresh in the shell context panel. */
import { EmptyState, MdButton, MdCard, Skeleton, UiIcon } from '@nyabula/ui';
import ContextSlot from '../../components/ContextSlot.vue';
import PluginCard from './PluginCard.vue';
import { usePluginsPage } from './PluginsView.logic';

const page = usePluginsPage();
</script>

<template>
  <div class="page">
    <div class="row between">
      <div>
        <h2 class="page-title">插件</h2>
        <p class="page-sub">设备上已安装的插件，点击进入插件页面</p>
      </div>
    </div>

    <div v-if="page.showSkeleton.value" class="grid-cards">
      <Skeleton v-for="i in 6" :key="i" height="110px" radius="var(--radius-l, 18px)" />
    </div>
    <EmptyState
      v-else-if="page.showError.value"
      tone="error"
      title="插件列表加载失败"
      :hint="page.error.value ?? undefined"
      action-text="重试"
      @action="page.refresh"
    />
    <EmptyState v-else-if="page.showEmpty.value" icon="extension" title="还没有插件" hint="设备上尚未安装任何插件" action-text="刷新" @action="page.refresh" />
    <div v-else class="grid-cards">
      <PluginCard v-for="p in page.items.value" :key="p.id" :plugin="p" @open="page.openPlugin(p.id)" />
    </div>

    <ContextSlot>
      <MdCard title="概览">
        <div class="stats">
          <div class="stat">
            <span class="stat-val">{{ page.stats.value.total }}</span>
            <span class="stat-label">插件数</span>
          </div>
          <div class="stat">
            <span class="stat-val ok">{{ page.stats.value.running }}</span>
            <span class="stat-label">运行中</span>
          </div>
          <div class="stat">
            <span class="stat-val" :class="{ warn: page.stats.value.missing > 0 }">{{ page.stats.value.missing }}</span>
            <span class="stat-label">权限缺失</span>
          </div>
        </div>
        <MdButton variant="tonal" :disabled="page.loading.value" class="refresh" @click="page.refresh">
          <span class="row" style="gap: 6px"><UiIcon name="refresh" :size="18" /> {{ page.loading.value ? '刷新中…' : '刷新列表' }}</span>
        </MdButton>
        <p v-if="!page.session.connected" class="muted hint">设备未连接，列表可能已过期</p>
      </MdCard>
    </ContextSlot>
  </div>
</template>

<style scoped>
.stats { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-bottom: 14px; }
.stat {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 4px;
  padding: 10px 6px;
  border-radius: var(--radius-m);
  background: var(--md-surface-container-high);
}
.stat-val { font: 700 22px var(--font-title); color: var(--md-on-surface); }
.stat-val.ok { color: var(--md-success); }
.stat-val.warn { color: var(--md-warning); }
.stat-label { font-size: 12px; color: var(--md-on-surface-variant); }
.refresh { width: 100%; }
.hint { font-size: 12px; margin: 10px 0 0; }
</style>
