<script setup lang="ts">
/* Nebula overview: rolling KPI numbers, 30-day relay frames, the device
 * constellation and a device status list.
 * Sources: 设备/在线/活跃客户端/今日转发帧 = GET /stats/overview;
 * 30 天转发帧 / 30 天在线时长 + the chart = sums of every device's
 * GET /devices/{id}/stats daily rows (see useOverview). */
import { computed } from 'vue';
import { EmptyState, Skeleton, UiIcon } from '@nyabula/ui';
import CountUp from '../../components/CountUp.vue';
import NebulaAreaChart from './NebulaAreaChart.vue';
import NebulaStarMap from './NebulaStarMap.vue';
import { useOverview } from '../../logic/useOverview';
import { useAuthStore } from '../../stores/auth';
import { useNavStore } from '../../stores/nav';
import { fmtAgo, fmtNum } from '../../logic/format';

const ov = useOverview();
const auth = useAuthStore();
const nav = useNavStore();

const kpis = computed(() => {
  const o = ov.overview.value;
  return [
    { k: '设备', v: o?.devices ?? null, icon: 'devices' },
    { k: '在线', v: o?.online ?? null, icon: 'link', acc: true },
    { k: '活跃客户端', v: o?.clients ?? null, icon: 'smartphone' },
    { k: '今日转发帧', v: o?.framesToday ?? null, icon: 'swap_horiz', fmt: fmtNum },
    { k: '30 天转发帧', v: ov.loaded.value ? ov.frames30.value : null, icon: 'history', fmt: fmtNum },
    { k: '30 天在线', v: ov.loaded.value ? ov.onlineHours30.value : null, icon: 'schedule', unit: 'h', dec: 1 },
  ];
});
const greeting = computed(() => {
  const h = new Date().getHours();
  return h < 6 ? '夜深了' : h < 11 ? '早上好' : h < 14 ? '中午好' : h < 18 ? '下午好' : '晚上好';
});
const summary = computed(() => {
  const o = ov.overview.value;
  if (!o) return ov.error.value ? '暂时连不上 Cloud。' : '正在同步…';
  if (!o.devices) return '还没有认领设备，从「设备」页开始吧。';
  return o.online ? `${o.online} 只猫在线，${o.clients} 个客户端正连着它们。` : '所有设备都在休息。';
});
</script>

<template>
  <div class="ov">
    <header class="hd nb-in">
      <p class="nb-kicker">总览</p>
      <h1 class="nb-title">{{ greeting }}，{{ auth.user?.name || '主人' }}</h1>
      <p class="sub">{{ summary }}</p>
      <p v-if="ov.error.value" class="nb-err"><UiIcon name="error" :size="15" /> {{ ov.error.value }}</p>
    </header>

    <section class="kpis">
      <div v-for="(k, i) in kpis" :key="k.k" class="kpi nb-glass" :style="{ animationDelay: i * 60 + 'ms' }">
        <span class="k-ico" :class="{ acc: k.acc }"><UiIcon :name="k.icon" :size="17" /></span>
        <div class="k-v" :class="{ acc: k.acc }">
          <template v-if="k.v !== null"><CountUp :to="k.v" :delay="200 + i * 60" :decimals="k.dec ?? 0" :format="k.fmt" /><small v-if="k.unit">{{ k.unit }}</small></template>
          <span v-else class="k-na">—</span>
        </div>
        <div class="k-k">{{ k.k }}</div>
      </div>
    </section>

    <section class="row2">
      <article class="panel nb-glass nb-in" style="--d: 200ms">
        <header class="p-hd">
          <div><h3>中继流量</h3><p>近 30 天 · 全部设备转发帧</p></div>
          <div class="legend"><span><i class="acc" />下行</span><span><i class="acc2" />上行</span></div>
        </header>
        <Skeleton v-if="!ov.loaded.value" :lines="4" />
        <NebulaAreaChart v-else-if="ov.hasTraffic.value" :rows="ov.daily.value" :height="250" />
        <p v-else class="nb-empty">近 30 天还没有经 Cloud 中继的流量。</p>
      </article>
      <article class="panel nb-glass nb-in" style="--d: 280ms">
        <header class="p-hd">
          <div><h3>设备星图</h3><p>点击节点查看设备</p></div>
          <button class="nb-btn" @click="nav.go('devices')">全部 <UiIcon name="arrow_forward" :size="15" /></button>
        </header>
        <NebulaStarMap :devices="ov.recent.value" @pick="nav.go('device', $event)" />
      </article>
    </section>

    <article class="panel nb-glass nb-in" style="--d: 360ms">
      <header class="p-hd"><div><h3>设备状态</h3><p>在线优先 · 按最后在线排序</p></div></header>
      <EmptyState v-if="ov.loaded.value && !ov.recent.value.length" icon="devices" title="还没有设备" hint="认领后会出现在这里" action-text="去认领" @action="nav.go('devices')" />
      <ol v-else class="list">
        <li v-for="(d, i) in ov.recent.value.slice(0, 6)" :key="d.deviceId" :style="{ animationDelay: 450 + i * 60 + 'ms' }" @click="nav.go('device', d.deviceId)">
          <span class="nb-dot" :class="{ on: d.online }" />
          <span class="l-t"><b>{{ d.name || d.deviceId }}</b><small class="nb-mono">{{ d.deviceId }} · core {{ d.coreVersion || '?' }}</small></span>
          <span class="l-at">{{ d.online ? '在线' : fmtAgo(d.lastSeen) }}</span>
          <UiIcon name="chevron_right" :size="18" class="chev" />
        </li>
      </ol>
    </article>
  </div>
</template>

<style scoped>
.ov { display: flex; flex-direction: column; gap: 18px; }
.hd h1 { font-size: clamp(28px, 3.4vw, 44px); margin: 6px 0 4px; }
.sub { color: var(--sub); margin: 0 0 4px; }
.kpis { display: grid; grid-template-columns: repeat(6, minmax(0, 1fr)); gap: 12px; }
.kpi { padding: 16px 18px; border-radius: 22px; animation: nbc-rise 0.8s var(--ease) both; transition: transform 0.45s var(--ease), border-color 0.3s; min-width: 0; }
.kpi:hover { transform: translateY(-3px); border-color: var(--hi); }
.k-ico { width: 30px; height: 30px; border-radius: 10px; display: grid; place-items: center; background: var(--wash); color: var(--sub); }
.k-ico.acc { color: var(--acc); background: rgba(var(--acc-rgb), 0.12); }
.k-v { font-size: 30px; line-height: 1.1; margin-top: 14px; letter-spacing: -0.01em; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.k-v.acc { color: var(--acc); }
.k-v small { font-size: 13px; color: var(--sub); margin-left: 3px; }
.k-na { color: var(--dim); }
.k-k { color: var(--sub); font-size: 12.5px; margin-top: 2px; }
.row2 { display: grid; grid-template-columns: minmax(0, 1.5fr) minmax(0, 1fr); gap: 18px; }
.panel { padding: 20px 22px; min-width: 0; }
.p-hd { display: flex; align-items: flex-start; justify-content: space-between; gap: 10px; margin-bottom: 12px; }
.p-hd h3 { margin: 0; font: 600 16px var(--font-body); }
.p-hd p { margin: 2px 0 0; color: var(--sub); font-size: 12px; }
.legend { display: flex; gap: 14px; font-size: 12px; color: var(--sub); }
.legend i { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 6px; }
.legend i.acc { background: var(--md-primary); }
.legend i.acc2 { background: var(--md-tertiary); }
.list { list-style: none; margin: 0; padding: 0; display: grid; }
.list li { display: flex; align-items: center; gap: 12px; padding: 11px 8px; border-bottom: 1px solid var(--line); border-radius: 12px; cursor: pointer; animation: nbc-rise 0.6s var(--ease) both; transition: background 0.2s; }
.list li:hover { background: var(--wash); }
.list li:last-child { border-bottom: none; }
.l-t { flex: 1; min-width: 0; display: flex; flex-direction: column; }
.l-t b { font-weight: 600; }
.l-t small { color: var(--sub); font-size: 11.5px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.l-at { color: var(--sub); font-size: 12px; white-space: nowrap; }
.chev { color: var(--dim); }
@media (max-width: 1180px) { .kpis { grid-template-columns: repeat(3, minmax(0, 1fr)); } .row2 { grid-template-columns: minmax(0, 1fr); } }
@media (max-width: 720px) { .kpis { grid-template-columns: repeat(2, minmax(0, 1fr)); } .k-v { font-size: 24px; } .panel { padding: 16px; } }
</style>
