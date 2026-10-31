<script setup lang="ts">
import DeviceLogin from '../../components/DeviceLogin.vue';
/* Nebula device detail drawer (open while nav.page === 'device'): current
 * state, 30-day online / frame bars (GET /devices/{id}/stats), the remote
 * panel over the Cloud relay (EyeCanvas + pairing + expression chips) and
 * a two-step unbind. The relay connects only while the drawer is open. */
import { computed, ref, watch } from 'vue';
import { EyeCanvas, PairOverlay, StatusPill, UiIcon } from '@nyabula/ui';
import { useNavStore } from '../../stores/nav';
import { useDeviceDetail } from '../../logic/useDeviceDetail';
import { useFleet } from '../../logic/useFleet';
import { dayLabel, fmtDate, fmtFrames, fmtHours } from '../../logic/format';

const nav = useNavStore();
const fleet = useFleet();
const openId = computed(() => (nav.page === 'device' ? nav.deviceId : null));
const dd = useDeviceDetail(openId);

const confirmUnbind = ref(false);
const unbinding = ref(false);
watch(openId, () => (confirmUnbind.value = false));

function close() {
  nav.go('devices');
}
async function unbind() {
  if (!openId.value) return;
  unbinding.value = true;
  await fleet.unclaim(openId.value, false);
  unbinding.value = false;
}

const cur = computed(() => dd.current.value?.current ?? null);
const maxOnline = computed(() => Math.max(3600, ...dd.series.value.map((d) => d.onlineSeconds)));
const maxFrames = computed(() => Math.max(1, ...dd.series.value.map((d) => d.framesUp + d.framesDown)));
const first = computed(() => dd.series.value[0]?.date ?? '');
const last = computed(() => dd.series.value[dd.series.value.length - 1]?.date ?? '');
</script>

<template>
  <Teleport to="body">
    <div class="nbc-portal">
      <Transition name="nb-fade"><div v-if="openId" class="scrim" @click="close" /></Transition>
      <Transition name="drawer">
        <aside v-if="openId" class="drawer nb-glass" role="dialog" :aria-label="dd.title.value">
          <header class="d-hd">
            <span class="nb-dot big" :class="{ on: cur?.online ?? dd.device.value?.online }" />
            <div class="d-t">
              <h2 class="nb-title">{{ dd.title.value }}</h2>
              <p class="nb-mono">{{ openId }} · core {{ dd.device.value?.coreVersion || '?' }}</p>
            </div>
            <button class="x" title="关闭" @click="close"><UiIcon name="close" :size="20" /></button>
          </header>

          <div class="facts">
            <div><small>状态</small><b :class="{ ok: cur?.online }">{{ cur ? (cur.online ? '在线' : '离线') : '—' }}</b></div>
            <div><small>客户端</small><b>{{ cur?.clients ?? '—' }}</b></div>
            <div><small>30 天在线率</small><b>{{ dd.rate.value !== null ? (dd.rate.value * 100).toFixed(1) + '%' : '—' }}</b></div>
            <div><small>认领于</small><b>{{ fmtDate(dd.device.value?.claimedAt ?? '') }}</b></div>
          </div>

          <section>
            <div class="s-hd">
              <h4>远程面板</h4>
              <StatusPill :tone="dd.pillTone.value" :label="dd.pillLabel.value" />
            </div>
            <div class="stage">
              <EyeCanvas v-if="dd.connected.value" :eye-state="dd.remote.lastEyeState" :clock-offset-ms="dd.remote.clockOffsetMs()" @look="dd.onLook" />
              <div v-else class="stage-off">
                <UiIcon :name="dd.remote.state === 'closed' ? 'link_off' : 'sync'" :size="26" :class="{ spin: dd.pillTone.value === 'busy' }" />
                <span>{{ dd.needPair.value ? '在设备眼睛上查看 6 位配对码' : '设备在线时会经 Cloud 中继自动接管' }}</span>
                <button v-if="dd.remote.state === 'closed'" class="nb-btn" @click="dd.reconnect()"><UiIcon name="refresh" :size="16" /> 重连</button>
              </div>
              <PairOverlay v-if="dd.showPairOverlay.value" :busy="dd.pairing.value" :error="dd.pairError.value" @submit="dd.doPair" />
        <DeviceLogin />
            </div>
            <p class="hint">点击 / 拖动画面 = 注视点（实时发 eye.look）</p>
            <div class="chips">
              <button v-for="m in dd.modes" :key="m" class="chip" :class="{ on: dd.activeMode.value === m }" :disabled="!dd.remote.canControl" @click="dd.pickMode(m)">
                {{ dd.MODE_LABELS[m] ?? m }}
              </button>
            </div>
            <p v-if="dd.remote.lastError" class="nb-err"><UiIcon name="error" :size="15" /> {{ dd.remote.lastError }}</p>
          </section>

          <section>
            <h4>每日在线时长</h4>
            <p v-if="dd.statsError.value" class="nb-err">{{ dd.statsError.value }}</p>
            <template v-else>
              <div class="bars">
                <i
                  v-for="(d, i) in dd.series.value"
                  :key="d.date"
                  :title="`${d.date} · ${fmtHours(d.onlineSeconds)}`"
                  :style="{ transform: `scaleY(${Math.max(0.02, d.onlineSeconds / maxOnline)})`, animationDelay: i * 18 + 'ms' }"
                />
              </div>
              <div class="bars-x"><span>{{ dayLabel(first) }}</span><span>{{ dayLabel(last) }}</span></div>
            </template>
          </section>

          <section>
            <h4>每日转发帧（上行 + 下行）· 30 天共 {{ fmtFrames(dd.frames30.value) }}</h4>
            <div v-if="!dd.statsError.value" class="bars alt">
              <i
                v-for="(d, i) in dd.series.value"
                :key="d.date"
                :title="`${d.date} · ↑${fmtFrames(d.framesUp)} ↓${fmtFrames(d.framesDown)}`"
                :style="{ transform: `scaleY(${Math.max(0.02, (d.framesUp + d.framesDown) / maxFrames)})`, animationDelay: i * 18 + 'ms' }"
              />
            </div>
          </section>

          <div class="d-actions">
            <Transition name="swap" mode="out-in">
              <button v-if="!confirmUnbind" key="a" class="nb-btn danger" @click="confirmUnbind = true"><UiIcon name="link_off" :size="16" /> 解绑设备</button>
              <div v-else key="b" class="confirm">
                <span>解绑后需重新输入认领码</span>
                <button class="nb-btn" @click="confirmUnbind = false">取消</button>
                <button class="nb-btn danger solid" :disabled="unbinding" @click="unbind">{{ unbinding ? '解绑中…' : '确认解绑' }}</button>
              </div>
            </Transition>
          </div>
        </aside>
      </Transition>
    </div>
  </Teleport>
</template>

<style scoped>
.scrim { position: fixed; inset: 0; z-index: 150; background: var(--md-scrim); backdrop-filter: blur(6px); }
.drawer {
  position: fixed;
  z-index: 160;
  top: 12px;
  right: 12px;
  bottom: 12px;
  width: min(540px, calc(100vw - 24px));
  padding: 24px;
  background: var(--panel-strong);
  border-radius: 28px;
  overflow-y: auto;
  overscroll-behavior: contain;
  display: flex;
  flex-direction: column;
  gap: 22px;
}
.drawer-enter-active { transition: transform 0.6s var(--ease), opacity 0.4s; }
.drawer-leave-active { transition: transform 0.35s var(--ease), opacity 0.3s; }
.drawer-enter-from, .drawer-leave-to { transform: translateX(40px); opacity: 0; }
.d-hd { display: flex; align-items: center; gap: 14px; }
.d-t { min-width: 0; }
.d-hd h2 { margin: 0; font-size: 26px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.d-hd p { margin: 2px 0 0; color: var(--sub); font-size: 12px; }
.nb-dot.big { width: 14px; height: 14px; }
.x { margin-left: auto; width: 38px; height: 38px; flex: none; border-radius: 50%; border: none; background: var(--wash); color: var(--sub); cursor: pointer; display: grid; place-items: center; transition: transform 0.4s var(--spring), color 0.2s; }
.x:hover { transform: rotate(90deg); color: var(--fg); }
.facts { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 8px; }
.facts div { padding: 12px; border-radius: 16px; background: var(--wash); display: flex; flex-direction: column; gap: 4px; min-width: 0; }
.facts small { color: var(--sub); font-size: 11.5px; }
.facts b { font-weight: 600; font-size: 15px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.facts b.ok { color: var(--acc); }
h4 { margin: 0 0 12px; font: 600 13px var(--font-body); color: var(--sub); }
.s-hd { display: flex; align-items: center; justify-content: space-between; gap: 10px; margin-bottom: 12px; }
.s-hd h4 { margin: 0; }
.stage { position: relative; height: 260px; border-radius: 22px; background: #000; overflow: hidden; border: 1px solid var(--line); box-shadow: 0 20px 50px -24px rgba(var(--acc-rgb), 0.5); }
.stage :deep(.eye-canvas) { border-radius: 0; }
.stage-off { position: absolute; inset: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 10px; color: #8e97b8; font-size: 12.5px; text-align: center; padding: 20px; }
.stage-off .nb-btn { color: #eef1ff; border-color: rgba(255, 255, 255, 0.15); background: rgba(255, 255, 255, 0.06); }
.spin { animation: sp 1.2s linear infinite; }
@keyframes sp { to { transform: rotate(360deg); } }
.hint { color: var(--sub); font-size: 11.5px; text-align: center; margin: 8px 0 12px; }
.chips { display: flex; flex-wrap: wrap; gap: 6px; }
.chip { border: 1px solid var(--line); background: var(--wash); color: var(--sub); border-radius: 999px; padding: 6px 12px; font: 600 12.5px var(--font-body); cursor: pointer; transition: all 0.25s var(--spring); }
.chip:hover:not(:disabled) { color: var(--fg); border-color: var(--hi); }
.chip.on { background: var(--acc); color: var(--md-on-primary); border-color: transparent; transform: scale(1.04); }
.chip:disabled { opacity: 0.45; cursor: default; }
.bars { display: flex; align-items: flex-end; gap: 3px; height: 100px; }
.bars i { flex: 1; height: 100%; border-radius: 3px 3px 1px 1px; background: linear-gradient(to top, rgba(var(--acc-rgb), 0.35), var(--acc)); transform-origin: bottom; animation: gb 0.8s var(--spring) both; transition: filter 0.2s; }
.bars.alt i { background: linear-gradient(to top, color-mix(in srgb, var(--md-tertiary) 35%, transparent), var(--md-tertiary)); }
.bars i:hover { filter: brightness(1.3); }
@keyframes gb { from { transform: scaleY(0); } }
.bars-x { display: flex; justify-content: space-between; color: var(--sub); font-size: 11px; margin-top: 6px; }
.d-actions { margin-top: auto; display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
.confirm { display: flex; align-items: center; gap: 8px; color: var(--md-error); flex-wrap: wrap; font-size: 13px; }
.swap-enter-active, .swap-leave-active { transition: opacity 0.2s, transform 0.3s var(--spring); }
.swap-enter-from, .swap-leave-to { opacity: 0; transform: scale(0.94); }
@media (max-width: 640px) {
  .drawer { top: auto; left: 8px; right: 8px; bottom: 8px; width: auto; max-height: calc(100dvh - 40px); padding: 18px; border-radius: 26px; }
  .drawer-enter-from, .drawer-leave-to { transform: translateY(40px); }
  .facts { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  .stage { height: 200px; }
}
</style>
