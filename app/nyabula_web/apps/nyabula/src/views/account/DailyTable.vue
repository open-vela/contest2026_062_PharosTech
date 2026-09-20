<script setup lang="ts">
/* Daily stats table (newest first). */
import { computed } from 'vue';
import type { DailyStat } from '../../api/cloud';
import { formatDuration } from './accountDevice.logic';

const props = defineProps<{ daily: DailyStat[] }>();
const rows = computed(() => [...props.daily].reverse());
</script>

<template>
  <div class="table-wrap">
    <table class="tbl">
      <thead>
        <tr><th>日期</th><th>在线时长</th><th>上行帧</th><th>下行帧</th></tr>
      </thead>
      <tbody>
        <tr v-for="d in rows" :key="d.date">
          <td class="mono">{{ d.date }}</td>
          <td>{{ formatDuration(d.onlineSeconds) }}</td>
          <td class="mono">{{ d.framesUp.toLocaleString() }}</td>
          <td class="mono">{{ d.framesDown.toLocaleString() }}</td>
        </tr>
        <tr v-if="!rows.length"><td colspan="4" class="muted" style="text-align: center">暂无数据</td></tr>
      </tbody>
    </table>
  </div>
</template>

<style scoped>
.table-wrap { overflow-x: auto; }
.tbl { width: 100%; border-collapse: collapse; font-size: 13.5px; color: var(--md-on-surface); }
.tbl th { text-align: left; font-weight: 600; font-size: 12px; color: var(--md-on-surface-variant); padding: 6px 8px; border-bottom: 1px solid var(--md-outline-variant); white-space: nowrap; }
.tbl td { padding: 8px; border-bottom: 1px solid var(--md-outline-variant); white-space: nowrap; }
.tbl tbody tr:last-child td { border-bottom: none; }
</style>
