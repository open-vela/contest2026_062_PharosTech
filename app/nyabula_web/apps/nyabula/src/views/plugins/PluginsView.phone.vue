<script setup lang="ts">
/* Phone: stats strip + list tiles; pull-down (touch drag past top) or the
 * refresh button reloads. */
import { ref } from 'vue';
import { EmptyState, MdButton, Skeleton, UiIcon } from '@nyabula/ui';
import { usePluginsPage } from './PluginsView.logic';
import { permSummary, stateLabel, stateTone } from './plugins.logic';

const page = usePluginsPage();

/* Minimal pull-to-refresh: track a downward drag when scrolled to the top. */
const PULL_THRESHOLD = 64;
const pull = ref(0);
let startY = 0;
let tracking = false;
function onTouchStart(e: TouchEvent) {
  const el = e.currentTarget as HTMLElement;
  const scroller = el.closest('[data-scroll]') ?? document.scrollingElement;
  if ((scroller?.scrollTop ?? 0) > 0) return;
  startY = e.touches[0]?.clientY ?? 0;
  tracking = true;
}
function onTouchMove(e: TouchEvent) {
  if (!tracking) return;
  const dy = (e.touches[0]?.clientY ?? 0) - startY;
  pull.value = dy > 0 ? Math.min(dy * 0.5, PULL_THRESHOLD + 20) : 0;
}
async function onTouchEnd() {
  if (!tracking) return;
  tracking = false;
  const fire = pull.value >= PULL_THRESHOLD;
  pull.value = 0;
  if (fire && !page.loading.value) await page.refresh();
}
</script>

<template>
  <div class="plugins-phone" @touchstart.passive="onTouchStart" @touchmove.passive="onTouchMove" @touchend="onTouchEnd" @touchcancel="onTouchEnd">
    <div class="pull" :style="{ height: pull + 'px', opacity: pull ? 1 : 0 }">
      <UiIcon name="refresh" :size="18" :class="{ spin: page.loading.value }" />
      <span>{{ pull >= 64 ? '松开刷新' : '下拉刷新' }}</span>
    </div>

    <div class="strip">
      <div class="chipstat"><b>{{ page.stats.value.total }}</b> 插件</div>
      <div class="chipstat ok"><b>{{ page.stats.value.running }}</b> 运行中</div>
      <div class="chipstat" :class="{ warn: page.stats.value.missing > 0 }"><b>{{ page.stats.value.missing }}</b> 权限缺失</div>
      <MdButton variant="icon" :disabled="page.loading.value" aria-label="刷新" class="refresh" @click="page.refresh">
        <UiIcon name="refresh" :size="20" :class="{ spin: page.loading.value }" />
      </MdButton>
    </div>

    <div v-if="page.showSkeleton.value" class="stack">
      <Skeleton v-for="i in 6" :key="i" height="66px" radius="var(--radius-m)" />
    </div>
    <EmptyState v-else-if="page.showError.value" tone="error" title="插件列表加载失败" :hint="page.error.value ?? undefined" action-text="重试" @action="page.refresh" />
    <EmptyState v-else-if="page.showEmpty.value" icon="extension" title="还没有插件" hint="设备上尚未安装任何插件" action-text="刷新" @action="page.refresh" />
    <div v-else class="stack">
      <button v-for="p in page.items.value" :key="p.id" class="list-tile" @click="page.openPlugin(p.id)">
        <span class="tile-icon"><UiIcon :name="p.icon ?? 'extension'" :size="20" /></span>
        <span class="tile-body">
          <span class="tile-title">{{ p.name }}</span>
          <span class="tile-sub">
            <span v-if="p.version" class="mono">v{{ p.version }} · </span>
            <template v-if="permSummary(p).total">已授 {{ permSummary(p).granted }}/{{ permSummary(p).total }}</template>
            <template v-else>无需权限</template>
          </span>
        </span>
        <span class="tile-trail">
          <span class="tag" :class="stateTone(p.state)">{{ stateLabel(p.state) }}</span>
          <UiIcon name="chevron_right" :size="18" />
        </span>
      </button>
    </div>
  </div>
</template>

<style scoped>
.plugins-phone { display: flex; flex-direction: column; gap: 12px; padding: 4px 14px calc(var(--shell-bottom) + 20px); }
.pull {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  overflow: hidden;
  font-size: 12.5px;
  color: var(--md-on-surface-variant);
  transition: opacity var(--dur-short, 0.15s) var(--ease-standard, ease);
}
.strip { display: flex; align-items: center; gap: 8px; overflow-x: auto; scrollbar-width: none; }
.strip::-webkit-scrollbar { display: none; }
.chipstat {
  flex: none;
  padding: 6px 12px;
  border-radius: 999px;
  background: var(--md-surface-container);
  color: var(--md-on-surface-variant);
  font-size: 12.5px;
}
.chipstat b { color: var(--md-on-surface); font-size: 14px; margin-right: 2px; }
.chipstat.ok b { color: var(--md-success); }
.chipstat.warn b { color: var(--md-warning); }
.refresh { margin-left: auto; flex: none; }
.list-tile { width: 100%; text-align: left; border: none; background: transparent; color: inherit; }
.spin { animation: spin 1s linear infinite; }
@keyframes spin { to { transform: rotate(360deg); } }
</style>
