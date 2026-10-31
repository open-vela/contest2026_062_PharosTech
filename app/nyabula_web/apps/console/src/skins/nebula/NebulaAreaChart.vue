<script setup lang="ts">
/* Daily relay frames (down / up) as two gradient areas with glowing lines
 * that draw in, plus a hover crosshair + tooltip. Hand-written SVG, fed
 * with real DailyStat rows (oldest first). */
import { computed, ref } from 'vue';
import type { DailyStat } from '../../api/client';
import { dayLabel, fmtHours, fmtNum } from '../../logic/format';

const props = withDefaults(defineProps<{ rows: DailyStat[]; height?: number }>(), { height: 240 });
const W = 720;
const H = computed(() => props.height);
const PAD = { l: 8, r: 8, t: 16, b: 26 };
const uid = `nbc${Math.random().toString(36).slice(2, 8)}`;

const n = computed(() => Math.max(2, props.rows.length));
const max = computed(() => Math.max(1, ...props.rows.map((d) => Math.max(d.framesDown, d.framesUp))) * 1.12);
const x = (i: number) => PAD.l + (i / (n.value - 1)) * (W - PAD.l - PAD.r);
const y = (v: number) => PAD.t + (1 - v / max.value) * (H.value - PAD.t - PAD.b);

function smooth(vals: number[]): string {
  if (!vals.length) return '';
  const pts = vals.map((v, i) => [x(i), y(v)] as const);
  let d = `M${pts[0][0]},${pts[0][1]}`;
  for (let i = 1; i < pts.length; i++) {
    const [x0, y0] = pts[i - 1];
    const [x1, y1] = pts[i];
    const cx = (x0 + x1) / 2;
    d += ` C${cx},${y0} ${cx},${y1} ${x1},${y1}`;
  }
  return d;
}
const base = computed(() => H.value - PAD.b);
const downLine = computed(() => smooth(props.rows.map((d) => d.framesDown)));
const upLine = computed(() => smooth(props.rows.map((d) => d.framesUp)));
const close = (line: string) => (line ? `${line} L${x(props.rows.length - 1)},${base.value} L${x(0)},${base.value} Z` : '');
const grid = computed(() => [0.25, 0.5, 0.75].map((f) => PAD.t + f * (H.value - PAD.t - PAD.b)));
const ticks = computed(() => {
  const last = props.rows.length - 1;
  return [0, Math.round(last / 4), Math.round(last / 2), Math.round((last * 3) / 4), last].filter((v, i, a) => v >= 0 && a.indexOf(v) === i);
});

const hover = ref<number | null>(null);
const svg = ref<SVGSVGElement | null>(null);
function onMove(e: PointerEvent) {
  const r = svg.value!.getBoundingClientRect();
  const px = ((e.clientX - r.left) / r.width) * W;
  const i = Math.round(((px - PAD.l) / (W - PAD.l - PAD.r)) * (n.value - 1));
  hover.value = Math.max(0, Math.min(props.rows.length - 1, i));
}
const tip = computed(() => (hover.value === null ? null : props.rows[hover.value]));
const tipLeft = computed(() => (hover.value === null ? 0 : (x(hover.value) / W) * 100));
</script>

<template>
  <div class="chart">
    <svg ref="svg" :viewBox="`0 0 ${W} ${H}`" preserveAspectRatio="none" :style="{ height: H + 'px' }" @pointermove="onMove" @pointerleave="hover = null">
      <defs>
        <linearGradient :id="`${uid}d`" x1="0" x2="0" y1="0" y2="1"><stop offset="0" class="s-acc" stop-opacity="0.35" /><stop offset="1" class="s-acc" stop-opacity="0" /></linearGradient>
        <linearGradient :id="`${uid}u`" x1="0" x2="0" y1="0" y2="1"><stop offset="0" class="s-acc2" stop-opacity="0.35" /><stop offset="1" class="s-acc2" stop-opacity="0" /></linearGradient>
        <filter :id="`${uid}g`" x="-10%" y="-50%" width="120%" height="200%"><feGaussianBlur stdDeviation="3" result="b" /><feMerge><feMergeNode in="b" /><feMergeNode in="SourceGraphic" /></feMerge></filter>
      </defs>
      <line v-for="g in grid" :key="g" :x1="0" :x2="W" :y1="g" :y2="g" class="grid" />
      <path :d="close(downLine)" :fill="`url(#${uid}d)`" class="area" />
      <path :d="close(upLine)" :fill="`url(#${uid}u)`" class="area a2" />
      <path :d="downLine" class="line l-acc" pathLength="1" :filter="`url(#${uid}g)`" />
      <path :d="upLine" class="line l-acc2 l2" pathLength="1" :filter="`url(#${uid}g)`" />
      <g v-if="hover !== null && tip">
        <line :x1="x(hover)" :x2="x(hover)" :y1="PAD.t" :y2="base" class="cross" />
        <circle :cx="x(hover)" :cy="y(tip.framesDown)" r="5" class="pt l-acc" />
        <circle :cx="x(hover)" :cy="y(tip.framesUp)" r="5" class="pt l-acc2" />
      </g>
      <text v-for="i in ticks" :key="i" :x="x(i)" :y="H - 6" class="lbl" :text-anchor="i === 0 ? 'start' : i === rows.length - 1 ? 'end' : 'middle'">{{ dayLabel(rows[i]?.date ?? '') }}</text>
    </svg>
    <Transition name="tip">
      <div v-if="tip" class="tip" :style="{ left: tipLeft + '%' }" :class="{ flip: tipLeft > 65 }">
        <b>{{ tip.date }}</b>
        <span><i class="k acc" />下行 {{ fmtNum(tip.framesDown) }}</span>
        <span><i class="k acc2" />上行 {{ fmtNum(tip.framesUp) }}</span>
        <span class="muted">在线 {{ fmtHours(tip.onlineSeconds) }}</span>
      </div>
    </Transition>
  </div>
</template>

<style scoped>
.chart { position: relative; }
svg { width: 100%; display: block; overflow: visible; cursor: crosshair; touch-action: pan-y; }
.s-acc { stop-color: var(--md-primary); }
.s-acc2 { stop-color: var(--md-tertiary); }
.grid { stroke: var(--line); stroke-dasharray: 3 5; }
.area { animation: fadeup 1.4s cubic-bezier(0.22, 1, 0.36, 1) 0.5s both; }
.a2 { animation-delay: 0.7s; }
@keyframes fadeup { from { opacity: 0; transform: translateY(12px); } }
.line { fill: none; stroke-width: 2.2; stroke-linecap: round; stroke-dasharray: 1; stroke-dashoffset: 1; animation: draw 1.8s cubic-bezier(0.22, 1, 0.36, 1) forwards; }
.l-acc { stroke: var(--md-primary); }
.l-acc2 { stroke: var(--md-tertiary); }
.l2 { animation-delay: 0.2s; }
@keyframes draw { to { stroke-dashoffset: 0; } }
.pt { fill: var(--md-surface); stroke-width: 2.5; }
.cross { stroke: var(--hi); stroke-dasharray: 3 3; }
.lbl { fill: var(--sub); font-size: 11px; font-family: var(--font-body); }
.tip {
  position: absolute;
  top: 0;
  transform: translateX(14px);
  pointer-events: none;
  display: flex;
  flex-direction: column;
  gap: 3px;
  padding: 10px 12px;
  border-radius: 14px;
  background: var(--panel-strong);
  border: 1px solid var(--line);
  backdrop-filter: blur(14px);
  font-size: 12px;
  white-space: nowrap;
  box-shadow: 0 14px 40px -12px rgba(0, 0, 0, 0.5);
  transition: left 0.12s ease-out;
}
.tip.flip { transform: translateX(calc(-100% - 14px)); }
.tip b { font-weight: 600; margin-bottom: 2px; }
.k { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 6px; }
.k.acc { background: var(--md-primary); }
.k.acc2 { background: var(--md-tertiary); }
.muted { color: var(--sub); }
.tip-enter-active, .tip-leave-active { transition: opacity 0.15s; }
.tip-enter-from, .tip-leave-to { opacity: 0; }
</style>
