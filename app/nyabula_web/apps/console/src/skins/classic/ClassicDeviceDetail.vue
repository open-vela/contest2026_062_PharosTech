<script setup lang="ts">
import DeviceLogin from '../../components/DeviceLogin.vue';
/* Device detail: stats cards + SVG bar charts + remote panel over the
 * Cloud relay (logic in useDeviceDetail, shared with the other skins). */
import { computed, toRef } from 'vue';
import { EyeCanvas, MdButton, MdCard, MdChip, PairOverlay, StatusPill } from '@nyabula/ui';
import { useDeviceDetail } from '../../logic/useDeviceDetail';
import { useFleet } from '../../logic/useFleet';
import { fmtFrames, fmtHours } from '../../logic/format';
import StatBars from './StatBars.vue';

const props = defineProps<{ deviceId: string }>();
const dd = useDeviceDetail(toRef(props, 'deviceId'));
const fleet = useFleet();
const nav = fleet.nav;
const current = computed(() => dd.current.value?.current ?? null);
</script>

<template>
  <div class="page">
    <div class="head">
      <MdButton variant="text" @click="nav.go('devices')">← 设备列表</MdButton>
      <h2 class="title">{{ dd.title.value }}</h2>
      <StatusPill :tone="dd.pillTone.value" :label="dd.pillLabel.value" />
    </div>

    <div class="grid">
      <MdCard title="日在线时长">
        <p v-if="dd.statsError.value" class="err">{{ dd.statsError.value }}</p>
        <StatBars v-else :items="dd.onlineBars.value.slice(-14)" :format="fmtHours" />
      </MdCard>
      <MdCard title="日转发帧数（上行 + 下行）">
        <p v-if="dd.statsError.value" class="err">{{ dd.statsError.value }}</p>
        <StatBars v-else :items="dd.frameBars.value.slice(-14)" :format="fmtFrames" color="var(--md-tertiary)" />
      </MdCard>
      <MdCard title="当前">
        <div class="now">
          <span class="now-item">
            状态：<b :class="{ ok: current?.online }">{{ current ? (current.online ? '在线' : '离线') : '—' }}</b>
          </span>
          <span class="now-item">连接客户端：<b>{{ current?.clients ?? '—' }}</b></span>
        </div>
        <MdButton variant="text" class="danger" @click="fleet.unclaim(deviceId)">解绑此设备</MdButton>
      </MdCard>
    </div>

    <MdCard title="远程面板" class="remote-card">
      <div class="canvas-box">
        <EyeCanvas v-if="dd.connected.value" :eye-state="dd.remote.lastEyeState" :clock-offset-ms="dd.remote.clockOffsetMs()" @look="dd.onLook" />
        <div v-else class="eye-placeholder" />
        <PairOverlay v-if="dd.showPairOverlay.value" :busy="dd.pairing.value" :error="dd.pairError.value" @submit="dd.doPair" />
        <DeviceLogin />
      </div>
      <p class="hint">点击 / 拖动画布 = 注视点（实时发 eye.look）</p>

      <div class="chips">
        <MdChip v-for="m in dd.modes" :key="m" :selected="dd.activeMode.value === m" :disabled="!dd.remote.canControl" @click="dd.pickMode(m)">
          {{ dd.MODE_LABELS[m] ?? m }}
        </MdChip>
      </div>
      <p v-if="!dd.connected.value && !dd.needPair.value" class="offline-hint">
        设备未接管（离线或连接中）。设备在线时会自动经 Cloud relay 建立 NyaLink。
      </p>
      <p v-if="dd.remote.lastError" class="err">{{ dd.remote.lastError }}</p>
    </MdCard>
  </div>
</template>

<style scoped>
.page { padding: 22px; max-width: 1080px; margin: 0 auto; }
.head { display: flex; align-items: center; gap: 14px; margin-bottom: 18px; flex-wrap: wrap; }
.title { font-size: 21px; flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap: 16px; margin-bottom: 16px; }
.now { display: flex; flex-direction: column; gap: 10px; font-size: 14px; color: var(--md-on-surface-variant); margin-bottom: 12px; }
.now-item b { color: var(--md-on-surface); }
.now-item b.ok { color: var(--md-primary); }
.danger { color: var(--md-error) !important; }
.remote-card { margin-top: 4px; }
.canvas-box { position: relative; height: 340px; margin-top: 4px; }
.eye-placeholder { width: 100%; height: 100%; border-radius: var(--radius-l); background: #000; }
.hint { color: var(--md-on-surface-variant); font-size: 12px; text-align: center; margin: 10px 0 14px; }
.chips { display: flex; flex-wrap: wrap; gap: 8px; }
.offline-hint { color: var(--md-on-surface-variant); font-size: 13px; margin: 14px 0 0; }
.err { color: var(--md-error); font-size: 13px; margin: 12px 0 0; }
@media (max-width: 640px) { .canvas-box { height: 240px; } }
</style>
