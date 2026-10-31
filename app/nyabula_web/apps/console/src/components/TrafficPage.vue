<script setup lang="ts">
import { computed, ref } from 'vue';
import { MdButton, MdCard } from '@nyabula/ui';
import { useOverview } from '../logic/useOverview';
import { dayLabel, fmtNum } from '../logic/format';
import StatBars from '../skins/classic/StatBars.vue';
const ov = useOverview(30);
const days = ref(30);
const rows = computed(() => ov.daily.value.slice(-days.value));
const up = computed(() => rows.value.reduce((s,r) => s+r.framesUp,0));
const down = computed(() => rows.value.reduce((s,r) => s+r.framesDown,0));
const hours = computed(() => rows.value.reduce((s,r) => s+r.onlineSeconds,0)/3600);
const frames = computed(() => rows.value.map(r => ({label:dayLabel(r.date),value:r.framesUp+r.framesDown})));
const online = computed(() => rows.value.map(r => ({label:dayLabel(r.date),value:r.onlineSeconds/3600})));
const failures = computed(() => ov.devices.devices.filter(d => ov.stats.errors[d.deviceId]).length);
</script>
<template>
  <div class="page traffic">
    <header><h1>流量</h1><div class="tools"><label>统计周期 <select v-model="days"><option :value="7">近 7 天</option><option :value="30">近 30 天</option></select></label><MdButton variant="outlined" :disabled="ov.loading.value" @click="ov.refresh">{{ ov.loading.value ? '刷新中…' : '刷新' }}</MdButton></div></header>
    <p v-if="ov.error.value || ov.devices.lastError" role="alert" class="error">{{ ov.error.value || ov.devices.lastError }}</p>
    <p v-if="failures" role="alert" class="error">{{ failures }} 台设备的统计读取失败，以下合计不完整。请刷新重试。</p>
    <div class="totals"><MdCard title="上行帧"><strong>{{ ov.loaded.value ? fmtNum(up) : '—' }}</strong></MdCard><MdCard title="下行帧"><strong>{{ ov.loaded.value ? fmtNum(down) : '—' }}</strong></MdCard><MdCard title="累计在线时长"><strong>{{ ov.loaded.value ? hours.toFixed(1) : '—' }}<small> 小时</small></strong></MdCard></div>
    <MdCard title="中继帧数"><StatBars :items="frames" :format="fmtNum" /><p v-if="ov.loaded.value && !ov.hasTraffic.value">这个周期还没有中继活动。</p></MdCard>
    <MdCard title="全部设备在线时长"><StatBars :items="online" :format="v => v.toFixed(1)+' 小时'" /></MdCard>
    <details><summary>查看每日明细</summary><div class="table-wrap"><table><thead><tr><th>日期</th><th>上行帧</th><th>下行帧</th><th>在线小时</th></tr></thead><tbody><tr v-for="r in rows" :key="r.date"><td>{{ r.date }}</td><td>{{ r.framesUp }}</td><td>{{ r.framesDown }}</td><td>{{ (r.onlineSeconds/3600).toFixed(2) }}</td></tr></tbody></table></div></details>
  </div>
</template>
<style scoped>
.traffic { display: flex; flex-direction: column; gap: 20px; padding: 24px; max-width: 1200px; margin: auto; }header,.tools { display: flex; align-items: center; gap: 14px; flex-wrap: wrap; }header { justify-content: space-between; }h1 { margin: 0; font: 30px var(--font-title); }.totals { display: grid; grid-template-columns: repeat(3,minmax(0,1fr)); gap: 16px; }strong { font-size: 32px; }small { font-size: 13px; font-weight: 400; }.error { color: var(--md-error); }select { padding: 8px; border-radius: var(--radius-s); border: 1px solid var(--md-outline-variant); color: var(--md-on-surface); background: var(--md-surface-container); }.table-wrap { overflow-x: auto; }table { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }td,th { padding: 12px; text-align: right; border-bottom: 1px solid var(--md-outline-variant); }summary { cursor: pointer; padding: 12px 0; }@media(max-width:600px){.totals { grid-template-columns: minmax(0,1fr); }.traffic { padding: 16px; }}
</style>
