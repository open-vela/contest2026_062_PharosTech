<script setup lang="ts">
/* Instrument overview: dot-matrix KPI strip, 30-day relay frames as dot
 * columns with a stepped online-hours line and hover crosshair, and a
 * fleet LED matrix (one row per device, one cell per day).
 * Sources: KPIs 01-04 = GET /stats/overview; 05-06, chart and matrix =
 * sums / ratios of GET /devices/{id}/stats daily rows (useOverview). */
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { UiIcon } from '@nyabula/ui';
import DotMatrix from '../../components/DotMatrix.vue';
import { useOverview } from '../../logic/useOverview';
import { useNavStore } from '../../stores/nav';
import { dayLabel, fmtHours, fmtNum } from '../../logic/format';

const ov = useOverview();
const nav = useNavStore();

const kpis = computed(() => {
  const o = ov.overview.value;
  const dash = '-';
  return [
    { k: '设备', en: 'DEVICES', v: o ? String(o.devices) : dash, tone: '' },
    { k: '在线', en: 'ONLINE', v: o ? String(o.online) : dash, tone: o?.online ? 'sig' : '' },
    { k: '活跃客户端', en: 'CLIENTS', v: o ? String(o.clients) : dash, tone: '' },
    { k: '今日转发帧', en: 'FRAMES 1D', v: o ? fmtNum(o.framesToday) : dash, tone: '' },
    { k: '30 天转发帧', en: 'FRAMES 30D', v: ov.loaded.value ? fmtNum(ov.frames30.value) : dash, tone: '' },
    { k: '30 天在线时长', en: 'ONLINE 30D', v: ov.loaded.value ? `${Math.round(ov.onlineHours30.value)}H` : dash, tone: '' },
  ];
});

/* ---- chart ---- */
const wrap = ref<HTMLElement | null>(null);
const W = ref(760);
const H = 220;
const PAD = { l: 44, r: 40, t: 14, b: 26 };
const ROWS = 16;
let ro: ResizeObserver | null = null;
onMounted(() => {
  ro = new ResizeObserver(([e]) => (W.value = Math.max(280, e.contentRect.width)));
  if (wrap.value) ro.observe(wrap.value);
});
onBeforeUnmount(() => ro?.disconnect());

const rows = computed(() => ov.daily.value);
const maxTotal = computed(() => Math.max(1, ...rows.value.map((d) => d.framesUp + d.framesDown)));
const maxHours = computed(() => Math.max(24, ...rows.value.map((d) => d.onlineSeconds / 3600)));
const chart = computed(() => {
  const n = Math.max(1, rows.value.length);
  const iw = W.value - PAD.l - PAD.r;
  const ih = H - PAD.t - PAD.b;
  const colW = iw / n;
  const pitch = ih / ROWS;
  const r = Math.max(1.3, Math.min(colW, pitch) * 0.32);
  const cols = rows.value.map((d, i) => {
    const total = d.framesUp + d.framesDown;
    const lit = total > 0 ? Math.max(1, Math.round((total / maxTotal.value) * ROWS)) : 0;
    const nUp = total > 0 ? Math.round((d.framesUp / total) * lit) : 0;
    const cx = PAD.l + colW * (i + 0.5);
    const dots = Array.from({ length: ROWS }, (_, j) => ({ cy: PAD.t + ih - pitch * (j + 0.5), s: j < nUp ? 'up' : j < lit ? 'dn' : 'off' }));
    return { cx, dots, d };
  });
  const y = (h: number) => PAD.t + ih - (h / maxHours.value) * ih;
  let path = '';
  rows.value.forEach((d, i) => {
    const x0 = PAD.l + colW * i;
    const hy = y(d.onlineSeconds / 3600);
    path += `${i === 0 ? 'M' : 'L'}${x0},${hy} L${x0 + colW},${hy} `;
  });
  return { cols, colW, r, path, ih, y };
});
const hover = ref<number | null>(null);
function move(e: PointerEvent) {
  const rect = (e.currentTarget as SVGElement).getBoundingClientRect();
  const i = Math.floor((e.clientX - rect.left - PAD.l) / chart.value.colW);
  hover.value = i >= 0 && i < rows.value.length ? i : null;
}
const hv = computed(() => (hover.value === null ? null : chart.value.cols[hover.value] ?? null));

/* ---- fleet matrix: daily online share per device ---- */
const matrix = computed(() =>
  ov.recent.value.map((d) => ({
    d,
    cells: ov.stats.series(d.deviceId, 30).map((x) => Math.min(1, x.onlineSeconds / 86400)),
    rate: ov.stats.onlineRate(d.deviceId),
  })),
);
</script>

<template>
  <div class="ov ins-boot">
    <section class="kpis">
      <div v-for="(k, i) in kpis" :key="k.en" class="kpi">
        <span class="ins-label"><span class="ins-idx">{{ String(i + 1).padStart(2, '0') }}</span> {{ k.en }}</span>
        <DotMatrix :text="k.v" :dot="5" :gap="1.8" :class="k.tone" />
        <span class="kpi-cn">{{ k.k }}</span>
      </div>
    </section>
    <p v-if="ov.error.value" class="ins-err">ERR  {{ ov.error.value }}</p>

    <section class="ins-panel">
      <div class="ins-panel-head">
        <span class="ins-label"><span class="ins-idx">A</span> 中继流量 RELAY FRAMES · 30D · ALL DEVICES</span>
        <span class="legend ins-label"><span><i class="lg up" /> UP</span><span><i class="lg dn" /> DOWN</span><span><i class="lg ln" /> ONLINE H</span></span>
      </div>
      <div ref="wrap" class="cw">
        <svg :width="W" :height="H" class="svg" @pointermove="move" @pointerleave="hover = null">
          <g class="grid">
            <line v-for="g in 5" :key="g" :x1="PAD.l" :x2="W - PAD.r" :y1="PAD.t + (chart.ih / 4) * (g - 1)" :y2="PAD.t + (chart.ih / 4) * (g - 1)" />
          </g>
          <text v-for="h in [0, maxHours / 2, maxHours]" :key="h" :x="W - PAD.r + 6" :y="chart.y(h) + 3" class="ax">{{ Math.round(h) }}H</text>
          <text :x="PAD.l - 6" :y="PAD.t + 6" class="ax end">{{ fmtNum(maxTotal) }}</text>
          <text :x="PAD.l - 6" :y="PAD.t + chart.ih + 3" class="ax end">0</text>
          <g v-for="(c, i) in chart.cols" :key="c.d.date" :class="{ hv: hover === i }">
            <circle v-for="(p, j) in c.dots" :key="j" :cx="c.cx" :cy="p.cy" :r="chart.r" :class="p.s" :style="{ animationDelay: i * 14 + j * 10 + 'ms' }" />
          </g>
          <path :d="chart.path" class="step" />
          <g v-if="hv">
            <line :x1="hv.cx" :x2="hv.cx" :y1="PAD.t" :y2="PAD.t + chart.ih" class="cross" />
          </g>
          <text v-for="i in [0, 10, 20, rows.length - 1]" :key="'t' + i" :x="chart.cols[i]?.cx ?? 0" :y="H - 6" class="ax mid">{{ dayLabel(rows[i]?.date ?? '') }}</text>
        </svg>
        <div v-if="hv" class="readout" :style="{ left: Math.min(W - 190, Math.max(0, hv.cx + 10)) + 'px' }">
          <span class="ins-label">{{ hv.d.date }}</span>
          <span class="ins-num">UP&nbsp;&nbsp;&nbsp;{{ hv.d.framesUp.toLocaleString() }}</span>
          <span class="ins-num">DOWN {{ hv.d.framesDown.toLocaleString() }}</span>
          <span class="ins-num ins-sig">ON&nbsp;&nbsp;&nbsp;{{ fmtHours(hv.d.onlineSeconds) }}</span>
        </div>
      </div>
      <p v-if="ov.loaded.value && !ov.hasTraffic.value" class="ins-label none">NO RELAY TRAFFIC · 近 30 天没有经 Cloud 中继的流量</p>
    </section>

    <section class="ins-panel">
      <div class="ins-panel-head">
        <span class="ins-label"><span class="ins-idx">B</span> 舰队在线矩阵 FLEET · DAILY ONLINE · 30D</span>
        <button class="ins-key" @click="nav.go('devices')">舰队 FLEET <UiIcon name="arrow_forward" :size="14" /></button>
      </div>
      <div v-if="matrix.length" class="mx">
        <button v-for="m in matrix" :key="m.d.deviceId" class="mx-row" @click="nav.go('device', m.d.deviceId)">
          <span class="mx-n"><span class="ins-led" :class="{ on: m.d.online }" /><b>{{ m.d.name || m.d.deviceId }}</b></span>
          <span class="mx-cells">
            <i v-for="(c, j) in m.cells" :key="j" :style="{ opacity: c > 0 ? 0.3 + c * 0.7 : 1 }" :class="{ on: c > 0 }" />
          </span>
          <span class="ins-num mx-r">{{ m.rate !== null ? (m.rate * 100).toFixed(0) + '%' : '--' }}</span>
        </button>
      </div>
      <p v-else-if="ov.loaded.value" class="ins-label none">NO DEVICES · 还没有认领设备</p>
    </section>
  </div>
</template>

<style scoped>
.ov { padding: 16px 20px 28px; display: flex; flex-direction: column; gap: 14px; max-width: 1480px; }
.kpis { display: grid; grid-template-columns: repeat(6, minmax(0, 1fr)); border: 1px solid var(--c-line); border-radius: 4px; background: var(--c-panel); }
.kpi { display: flex; flex-direction: column; gap: 12px; padding: 14px; border-right: 1px solid var(--c-line); min-width: 0; overflow: hidden; color: var(--c-fg); }
.kpi:last-child { border-right: none; }
.kpi :deep(.dm) { max-width: 100%; height: auto; }
.kpi .sig { color: var(--c-sig); }
.kpi-cn { font-size: 12px; color: var(--c-dim); }
.legend { display: flex; gap: 14px; }
.legend span { display: inline-flex; align-items: center; gap: 6px; }
.lg { display: inline-block; width: 8px; height: 8px; border-radius: 50%; }
.lg.up { background: var(--c-sig); }
.lg.dn { background: var(--c-fg); }
.lg.ln { border-radius: 0; height: 2px; background: var(--c-warn); }
.cw { position: relative; width: 100%; overflow: hidden; }
.svg { display: block; cursor: crosshair; touch-action: pan-y; }
.grid line { stroke: var(--c-line); stroke-dasharray: 2 4; }
.ax { fill: var(--c-mute); font: 600 9.5px var(--c-mono); }
.ax.end { text-anchor: end; }
.ax.mid { text-anchor: middle; }
circle { animation: dot 300ms steps(3, jump-none) both; }
@keyframes dot { from { opacity: 0; } }
circle.off { fill: var(--c-line); }
circle.up { fill: var(--c-sig); }
circle.dn { fill: var(--c-fg); opacity: 0.85; }
.hv circle.off { fill: var(--c-line-2); }
.step { fill: none; stroke: var(--c-warn); stroke-width: 1.5; stroke-linejoin: miter; }
.cross { stroke: var(--c-fg); stroke-width: 1; stroke-dasharray: 3 3; opacity: 0.6; }
.readout { position: absolute; top: 8px; display: flex; flex-direction: column; gap: 3px; padding: 8px 10px; background: var(--c-bg); border: 1px solid var(--c-line-2); border-radius: 3px; pointer-events: none; min-width: 170px; }
.readout .ins-num { font-size: 11.5px; }
.none { margin: 10px 0 0; text-align: center; }
.mx { display: flex; flex-direction: column; }
.mx-row { display: grid; grid-template-columns: minmax(120px, 200px) minmax(0, 1fr) 44px; gap: 14px; align-items: center; padding: 7px 4px; border: none; border-top: 1px solid var(--c-line); background: none; color: var(--c-fg); cursor: pointer; text-align: left; transition: background 120ms; }
.mx-row:first-child { border-top: none; }
.mx-row:hover { background: var(--c-panel-2); }
.mx-n { display: flex; align-items: center; gap: 8px; min-width: 0; }
.mx-n b { font-weight: 600; font-size: 13px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.mx-cells { display: grid; grid-template-columns: repeat(30, minmax(0, 1fr)); gap: 2px; }
.mx-cells i { height: 12px; background: var(--c-line); border-radius: 1px; }
.mx-cells i.on { background: var(--c-sig); }
.mx-r { text-align: right; font-size: 12px; }
@media (max-width: 1180px) { .kpis { grid-template-columns: repeat(3, minmax(0, 1fr)); } .kpi:nth-child(3) { border-right: none; } .kpi:nth-child(-n + 3) { border-bottom: 1px solid var(--c-line); } }
@media (max-width: 720px) {
  .ov { padding: 12px; }
  .kpis { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  .kpi { border-right: 1px solid var(--c-line) !important; border-bottom: 1px solid var(--c-line); }
  .kpi:nth-child(2n) { border-right: none !important; }
  .legend { display: none; }
  .mx-row { grid-template-columns: minmax(0, 1fr) 40px; }
  .mx-cells { grid-column: 1 / -1; order: 3; }
}
</style>
