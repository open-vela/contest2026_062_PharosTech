<script setup lang="ts">
/* Desktop: header strip, then chart | table two columns; device card in the
 * shell context panel. */
import { EmptyState, MdButton, MdCard, Skeleton, UiIcon } from '@nyabula/ui';
import ContextSlot from '../../components/ContextSlot.vue';
import DeviceStatsHeader from './DeviceStatsHeader.vue';
import OnlineChart from './OnlineChart.vue';
import DailyTable from './DailyTable.vue';
import { useAccountDevicePage } from './accountDevice.logic';

const props = defineProps<{ id: string }>();
const page = useAccountDevicePage(props.id);
</script>

<template>
  <div class="page dev-desktop">
    <div class="row between" style="margin-bottom: 14px">
      <div>
        <h1 class="page-title">{{ page.title.value }}</h1>
        <p class="page-sub mono" style="margin: 0">{{ page.id }}</p>
      </div>
      <MdButton variant="text" @click="page.back()"><UiIcon name="arrow_back" :size="16" /> 返回账号</MdButton>
    </div>

    <Skeleton v-if="page.loader.busy.value && !page.stats.value" :lines="4" />
    <EmptyState v-else-if="!page.stats.value" tone="error" icon="error" title="无法加载统计" hint="检查登录状态与网络后重试" action-text="重试" @action="page.reload()" />
    <template v-else>
      <DeviceStatsHeader :page="page" />
      <div class="two">
        <MdCard title="每日在线时长"><OnlineChart :daily="page.daily.value" :height="260" /></MdCard>
        <MdCard title="每日明细"><DailyTable :daily="page.daily.value" /></MdCard>
      </div>
    </template>

    <ContextSlot>
      <MdCard title="设备">
        <dl class="kv">
          <div><dt>名称</dt><dd>{{ page.device.value?.name || '—' }}</dd></div>
          <div><dt>ID</dt><dd class="mono">{{ page.id }}</dd></div>
          <div><dt>Core</dt><dd>{{ page.device.value?.coreVersion || '—' }}</dd></div>
          <div><dt>认领于</dt><dd>{{ page.device.value?.claimedAt ? new Date(page.device.value.claimedAt).toLocaleDateString() : '—' }}</dd></div>
          <div><dt>天数</dt><dd>{{ page.daily.value.length }}</dd></div>
        </dl>
        <div class="row" style="margin-top: 14px">
          <MdButton variant="outlined" @click="page.reload()"><UiIcon name="refresh" :size="16" /> 刷新</MdButton>
        </div>
      </MdCard>
    </ContextSlot>
  </div>
</template>

<style scoped>
.two { display: grid; grid-template-columns: minmax(0, 1.3fr) minmax(320px, 1fr); gap: 14px; margin-top: 14px; align-items: start; }
</style>
