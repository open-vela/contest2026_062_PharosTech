<script setup lang="ts">
/* Tablet: stacked header -> chart -> table (single column both orientations;
 * landscape gets a taller chart). */
import { computed, inject } from 'vue';
import { EmptyState, MdButton, MdCard, Skeleton, UiIcon } from '@nyabula/ui';
import DeviceStatsHeader from './DeviceStatsHeader.vue';
import OnlineChart from './OnlineChart.vue';
import DailyTable from './DailyTable.vue';
import { useAccountDevicePage } from './accountDevice.logic';
import type { useFormFactor } from '../../composables/useFormFactor';

const props = defineProps<{ id: string }>();
const page = useAccountDevicePage(props.id);
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const chartH = computed(() => (ff.orientation.value === 'landscape' ? 240 : 200));
</script>

<template>
  <div class="page dev-tablet">
    <div class="row between" style="margin-bottom: 12px">
      <div>
        <h1 class="page-title">{{ page.title.value }}</h1>
        <p class="page-sub mono" style="margin: 0">{{ page.id }}</p>
      </div>
      <MdButton variant="icon" @click="page.reload()"><UiIcon name="refresh" :size="20" /></MdButton>
    </div>

    <Skeleton v-if="page.loader.busy.value && !page.stats.value" :lines="4" />
    <EmptyState v-else-if="!page.stats.value" tone="error" icon="error" title="无法加载统计" hint="检查登录状态与网络后重试" action-text="重试" @action="page.reload()" />
    <div v-else class="stack" style="gap: 14px">
      <DeviceStatsHeader :page="page" />
      <MdCard title="每日在线时长"><OnlineChart :daily="page.daily.value" :height="chartH" /></MdCard>
      <MdCard title="每日明细"><DailyTable :daily="page.daily.value" /></MdCard>
    </div>
  </div>
</template>
