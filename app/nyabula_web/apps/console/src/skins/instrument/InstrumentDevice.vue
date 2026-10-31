<script setup lang="ts">
import DeviceLogin from '../../components/DeviceLogin.vue';
/* Instrument device page: readouts (GET /devices/{id}/stats current),
 * the remote monitor over the Cloud relay (EyeCanvas framed with corner
 * ticks, pairing overlay, expression key matrix, gaze readout) and the
 * 30-day online / frame columns. Hold-to-unclaim lives here too. */
import { computed, onBeforeUnmount, ref, toRef } from 'vue';
import { EyeCanvas, PairOverlay, UiIcon } from '@nyabula/ui';
import DotMatrix from '../../components/DotMatrix.vue';
import { useDeviceDetail } from '../../logic/useDeviceDetail';
import { useFleet } from '../../logic/useFleet';
import { dayLabel, fmtDate, fmtFrames, fmtHours, fmtNum } from '../../logic/format';

const props = defineProps<{ deviceId: string }>();
const dd = useDeviceDetail(toRef(props, 'deviceId'));
const fleet = useFleet();
const nav = fleet.nav;

const cur = computed(() => dd.current.value?.current ?? null);
const STATE_EN: Record<string, string> = {
  idle: 'IDLE', connecting: 'CONNECTING', authenticating: 'AUTH', 'pairing-required': 'PAIR', connected: 'LINKED', reconnecting: 'RETRY', closed: 'CLOSED',
};
const linkEn = computed(() => STATE_EN[dd.remote.state] ?? String(dd.remote.state).toUpperCase());

/* Gaze readout: last look sent from the canvas (normalized -1..1). */
const gaze = ref<{ x: number; y: number } | null>(null);
function onLook(d: { x?: number; y?: number; release?: boolean }) {
  gaze.value = d.release ? null : { x: d.x ?? 0, y: d.y ?? 0 };
  dd.onLook(d);
}

const maxOnline = computed(() => Math.max(3600, ...dd.series.value.map((d) => d.onlineSeconds)));
const maxFrames = computed(() => Math.max(1, ...dd.series.value.map((d) => d.framesUp + d.framesDown)));

/* hold to unclaim */
const holding = ref(false);
let holdT = 0;
function holdStart() {
  holding.value = true;
  holdT = window.setTimeout(() => {
    holding.value = false;
    void fleet.unclaim(props.deviceId, false);
  }, 1500);
}
function holdEnd() {
  clearTimeout(holdT);
  holding.value = false;
}
onBeforeUnmount(() => clearTimeout(holdT));
</script>

<template>
  <div class="dev ins-boot">
    <header class="hd">
      <button class="ins-key" @click="nav.go('devices')"><UiIcon name="arrow_back" :size="15" /> 舰队</button>
      <div class="hd-t">
        <h2>{{ dd.title.value }}</h2>
        <span class="ins-label">{{ deviceId }} · CORE {{ dd.device.value?.coreVersion || '--' }} · CLAIMED {{ fmtDate(dd.device.value?.claimedAt ?? '') }}</span>
      </div>
    </header>

    <section class="rds">
      <div class="rd"><span class="ins-label">STATUS</span><span class="ins-num" :class="cur?.online ? 'ins-sig' : 'ins-dim'"><span class="ins-led" :class="{ on: cur?.online }" /> {{ cur ? (cur.online ? 'ONLINE' : 'OFFLINE') : '--' }}</span></div>
      <div class="rd"><span class="ins-label">CLIENTS</span><DotMatrix :text="cur ? String(cur.clients) : '-'" :dot="3.4" :gap="1.2" /></div>
      <div class="rd"><span class="ins-label">ONLINE 30D</span><DotMatrix :text="dd.rate.value !== null ? (dd.rate.value * 100).toFixed(0) + '%' : '-'" :dot="3.4" :gap="1.2" /></div>
      <div class="rd"><span class="ins-label">FRAMES 30D</span><DotMatrix :text="fmtNum(dd.frames30.value)" :dot="3.4" :gap="1.2" /></div>
      <div class="rd"><span class="ins-label">RELAY LINK</span><span class="ins-num" :class="{ 'ins-sig': dd.connected.value, 'ins-warn': dd.remote.state === 'closed' }">{{ linkEn }}</span></div>
    </section>

    <section class="ins-panel mon-p">
      <div class="ins-panel-head">
        <span class="ins-label"><span class="ins-idx">A</span> 远程监视 REMOTE MONITOR · NYALINK VIA RELAY</span>
        <span class="ins-label gz">GAZE X {{ gaze ? gaze.x.toFixed(2) : '---' }} · Y {{ gaze ? gaze.y.toFixed(2) : '---' }}</span>
      </div>
      <div class="mon">
        <i class="cn tl" /><i class="cn tr" /><i class="cn bl" /><i class="cn br" />
        <EyeCanvas v-if="dd.connected.value" :eye-state="dd.remote.lastEyeState" :clock-offset-ms="dd.remote.clockOffsetMs()" @look="onLook" />
        <div v-else class="mon-off">
          <DotMatrix :text="linkEn" :dot="4" :gap="1.5" />
          <span class="ins-label">{{ dd.needPair.value ? '在设备眼睛上查看 6 位配对码' : '设备在线时经 Cloud 中继自动接管' }}</span>
          <button v-if="dd.remote.state === 'closed'" class="ins-key" @click="dd.reconnect()"><UiIcon name="refresh" :size="14" /> 重连 RETRY</button>
        </div>
        <PairOverlay v-if="dd.showPairOverlay.value" :busy="dd.pairing.value" :error="dd.pairError.value" @submit="dd.doPair" />
        <DeviceLogin />
      </div>
      <div class="keys">
        <button v-for="(m, i) in dd.modes" :key="m" class="ins-key mk" :class="{ on: dd.activeMode.value === m }" :disabled="!dd.remote.canControl" @click="dd.pickMode(m)">
          <span class="ins-idx">{{ String(i + 1).padStart(2, '0') }}</span>{{ dd.MODE_LABELS[m] ?? m }}
        </button>
      </div>
      <p v-if="dd.remote.lastError" class="ins-err">ERR  {{ dd.remote.lastError }}</p>
    </section>

    <section class="two">
      <div class="ins-panel">
        <div class="ins-panel-head"><span class="ins-label"><span class="ins-idx">B</span> 在线时长 ONLINE H · 30D</span></div>
        <p v-if="dd.statsError.value" class="ins-err">ERR  {{ dd.statsError.value }}</p>
        <div v-else class="cols">
          <i v-for="(d, i) in dd.series.value" :key="d.date" :title="`${d.date} · ${fmtHours(d.onlineSeconds)}`" :style="{ transform: `scaleY(${Math.max(0.03, d.onlineSeconds / maxOnline)})`, animationDelay: i * 12 + 'ms' }" />
        </div>
        <div class="xs ins-label"><span>{{ dayLabel(dd.series.value[0]?.date ?? '') }}</span><span>{{ dayLabel(dd.series.value[dd.series.value.length - 1]?.date ?? '') }}</span></div>
      </div>
      <div class="ins-panel">
        <div class="ins-panel-head"><span class="ins-label"><span class="ins-idx">C</span> 转发帧 FRAMES UP+DOWN · 30D</span></div>
        <div v-if="!dd.statsError.value" class="cols alt">
          <i v-for="(d, i) in dd.series.value" :key="d.date" :title="`${d.date} · ↑${fmtFrames(d.framesUp)} ↓${fmtFrames(d.framesDown)}`" :style="{ transform: `scaleY(${Math.max(0.03, (d.framesUp + d.framesDown) / maxFrames)})`, animationDelay: i * 12 + 'ms' }" />
        </div>
        <div class="xs ins-label"><span>{{ dayLabel(dd.series.value[0]?.date ?? '') }}</span><span>{{ dayLabel(dd.series.value[dd.series.value.length - 1]?.date ?? '') }}</span></div>
      </div>
    </section>

    <section class="danger-row">
      <button class="ins-key danger hold" :class="{ holding }" @pointerdown="holdStart" @pointerup="holdEnd" @pointerleave="holdEnd" @pointercancel="holdEnd" @keydown.enter.prevent="fleet.unclaim(deviceId)">
        <span class="fill" />
        <span class="hl">{{ holding ? '松开取消…' : '按住 1.5s 解绑 UNCLAIM' }}</span>
      </button>
    </section>
  </div>
</template>

<style scoped>
.dev { padding: 16px 20px 28px; max-width: 1480px; display: flex; flex-direction: column; gap: 14px; }
.hd { display: flex; align-items: center; gap: 14px; flex-wrap: wrap; }
.hd-t { display: flex; flex-direction: column; gap: 4px; min-width: 0; }
.hd-t h2 { margin: 0; font: 400 24px var(--font-title); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.hd-t .ins-label { text-transform: none; letter-spacing: 0.06em; }
.rds { display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); border: 1px solid var(--c-line); border-radius: 4px; background: var(--c-panel); }
.rd { display: flex; flex-direction: column; gap: 10px; padding: 12px 14px; border-right: 1px solid var(--c-line); min-width: 0; overflow: hidden; color: var(--c-fg); }
.rd:last-child { border-right: none; }
.rd .ins-num { display: inline-flex; align-items: center; gap: 8px; }
.rd :deep(.dm) { max-width: 100%; height: auto; }
.gz { font-variant-numeric: tabular-nums; }
.mon { position: relative; height: 340px; background: #000; border: 1px solid var(--c-line-2); border-radius: 2px; }
.mon :deep(.eye-canvas) { border-radius: 0; }
.cn { position: absolute; width: 14px; height: 14px; border-color: var(--c-sig); border-style: solid; border-width: 0; z-index: 2; pointer-events: none; }
.cn.tl { top: 6px; left: 6px; border-top-width: 2px; border-left-width: 2px; }
.cn.tr { top: 6px; right: 6px; border-top-width: 2px; border-right-width: 2px; }
.cn.bl { bottom: 6px; left: 6px; border-bottom-width: 2px; border-left-width: 2px; }
.cn.br { bottom: 6px; right: 6px; border-bottom-width: 2px; border-right-width: 2px; }
.mon-off { position: absolute; inset: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 14px; color: #e9e9e6; text-align: center; padding: 16px; }
.mon-off .ins-label { color: #8a8a84; text-transform: none; letter-spacing: 0.04em; }
.mon-off .ins-key { color: #e9e9e6; background: #1b1b1e; border-color: #48484b; }
.keys { display: grid; grid-template-columns: repeat(auto-fill, minmax(104px, 1fr)); gap: 6px; margin-top: 12px; }
.mk { justify-content: flex-start; font-family: var(--font-body); letter-spacing: 0.02em; text-transform: none; font-size: 12.5px; }
.mk .ins-idx { font: 600 9.5px var(--c-mono); margin-right: 2px; }
.mk.on .ins-idx { color: var(--c-sig); }
.two { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 14px; }
.cols { display: flex; align-items: flex-end; gap: 2px; height: 90px; }
.cols i { flex: 1; height: 100%; background: var(--c-sig); transform-origin: bottom; border-radius: 1px; animation: grow 420ms var(--c-ease) both; }
.cols.alt i { background: var(--c-fg); }
@keyframes grow { from { transform: scaleY(0); } }
.xs { display: flex; justify-content: space-between; margin-top: 6px; }
.danger-row { display: flex; justify-content: flex-end; }
.hold { overflow: hidden; min-width: 200px; touch-action: none; }
.hold .fill { position: absolute; inset: 0; background: var(--c-warn); transform: scaleX(0); transform-origin: left; transition: transform 200ms ease-out; }
.hold.holding .fill { transform: scaleX(1); transition: transform 1.5s linear; }
.hold .hl { position: relative; }
.hold.holding { color: var(--md-on-error); }
@media (max-width: 1000px) { .rds { grid-template-columns: repeat(3, minmax(0, 1fr)); } .rd:nth-child(3) { border-right: none; } .rd:nth-child(-n + 3) { border-bottom: 1px solid var(--c-line); } }
@media (max-width: 720px) {
  .dev { padding: 12px; }
  .rds { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  .rd { border-right: 1px solid var(--c-line) !important; border-bottom: 1px solid var(--c-line); }
  .rd:nth-child(2n) { border-right: none !important; }
  .mon { height: 230px; }
  .two { grid-template-columns: minmax(0, 1fr); }
  .gz { display: none; }
  .danger-row { justify-content: stretch; }
  .hold { width: 100%; }
}
</style>
