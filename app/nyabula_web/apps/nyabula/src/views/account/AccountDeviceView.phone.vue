<script setup lang="ts">
/* Phone: compact status cards, chart, then the table. */
import { EmptyState, MdCard, Skeleton } from '@nyabula/ui';
import DeviceStatsHeader from './DeviceStatsHeader.vue';
import OnlineChart from './OnlineChart.vue';
import DailyTable from './DailyTable.vue';
import { useAccountDevicePage } from './accountDevice.logic';

const props = defineProps<{ id: string }>();
const page = useAccountDevicePage(props.id);
</script>

<template>
  <div class="dev-phone">
    <p class="muted mono sub">{{ page.title.value }} · {{ page.id }}</p>
    <Skeleton v-if="page.loader.busy.value && !page.stats.value" :lines="4" />
    <EmptyState v-else-if="!page.stats.value" tone="error" icon="error" title="无法加载统计" hint="检查登录状态与网络后重试" action-text="重试" @action="page.reload()" />
    <template v-else>
      <DeviceStatsHeader :page="page" compact />
      <MdCard title="每日在线时长"><OnlineChart :daily="page.daily.value" :height="180" /></MdCard>
      <MdCard title="每日明细"><DailyTable :daily="page.daily.value" /></MdCard>
    </template>
  </div>
</template>

<style scoped>
.dev-phone { display: flex; flex-direction: column; gap: 12px; padding: 12px 14px 32px; }
.sub { font-size: 12px; margin: 0; word-break: break-all; }
</style>
