<script setup lang="ts">
/* Network: WiFi status from sys.info + network.status; scan / switch share
 * useWifiSetup() with the provisioning page. */
import { computed, onBeforeUnmount, onMounted, watch } from 'vue';
import { EmptyState, MdButton, MdCard, Skeleton, UiIcon, useDialogStore } from '@nyabula/ui';
import { rssiBars, useSysInfo, wifiOf } from './sysinfo';
import { useDeviceRuntime } from '../../../composables/useDeviceRuntime';
import { useWifiSetup } from '../../../composables/useWifiSetup';
import { NETWORK_STATE_LABEL } from '../../../lib/wifi';
import WifiSetupForm from '../../../components/WifiSetupForm.vue';
import WifiJoinNotice from '../../../components/WifiJoinNotice.vue';

/** The device refreshes its signal reading every 10 s; poll at the same pace. */
const SIGNAL_POLL_MS = 10000;

const { session, task, info } = useSysInfo();
const device = useDeviceRuntime();
const setup = useWifiSetup();
const dialog = useDialogStore();
const net = computed(() => setup.status.value);
/* Signal: network.status `rssi` (live, dBm) first, sys.info `wifi.rssi` as the
 * fallback for firmware that only reports it there. */
const wifi = computed(() => {
  const fromInfo = wifiOf(info.value);
  const wireless = device.snapshot?.network.interfaces.find(item => item.ssid);
  const online = net.value?.state === 'sta_online';
  return {
    ssid: wireless?.ssid ?? (online ? net.value?.ssid : null) ?? fromInfo.ssid,
    rssi: net.value?.rssi ?? fromInfo.rssi,
  };
});
watch(() => setup.session.connected, (c) => { if (c) void setup.refreshStatus(); }, { immediate: true });

/* Keep the signal fresh while this section is on screen. The joining phase
 * runs its own faster poll, so stay out of its way. */
let signalTimer: number | undefined;
async function pollSignal(): Promise<void> {
  if (document.hidden || !session.connected || setup.phase.value === 'joining') return;
  const status = await setup.refreshStatus(true);
  if (status && status.rssi === null && status.state === 'sta_online') {
    try {
      task.data.value = (await session.request('sys.info')) as typeof task.data.value;
    } catch { /* keep the last reading */ }
  }
}
function onVisibility(): void {
  if (!document.hidden) void pollSignal();
}
onMounted(() => {
  signalTimer = window.setInterval(() => void pollSignal(), SIGNAL_POLL_MS);
  document.addEventListener('visibilitychange', onVisibility);
});
onBeforeUnmount(() => {
  window.clearInterval(signalTimer);
  document.removeEventListener('visibilitychange', onVisibility);
});

async function forgetWifi(): Promise<void> {
  if (await dialog.confirm('设备会断开当前 WiFi 并回到配网热点，需要重新扫描设备上的二维码来配置。', { title: '清除保存的 WiFi？', danger: true, confirmText: '清除' })) {
    await setup.forget();
  }
}
const quality = computed(() => ['无信号', '弱', '一般', '良好', '优秀'][rssiBars(wifi.value.rssi)]);
</script>

<template>
  <div class="stack">
    <MdCard title="WiFi 状态">
      <Skeleton v-if="task.busy.value && !info && !net" :lines="3" />
      <EmptyState v-else-if="!info && !net" tone="error" compact title="读取失败" action-text="重试" @action="task.run()" />
      <div v-else class="wifi">
        <span class="orb"><UiIcon name="wifi" :size="26" /></span>
        <div class="wifi-body">
          <div class="ssid">{{ wifi.ssid ?? 'WiFi 关联状态未提供' }}</div>
          <div class="muted sub">
            <template v-if="wifi.rssi !== null">
              <span class="bars" :title="`${rssiBars(wifi.rssi)} / 4 格`"><i v-for="n in 4" :key="n" :class="{ on: n <= rssiBars(wifi.rssi) }" /></span>
              信号 {{ quality }} · {{ wifi.rssi }} dBm
            </template>
            <template v-else>无信号数据</template>
          </div>
        </div>
        <span class="tag" :class="wifi.ssid ? 'ok' : 'warn'">{{ wifi.ssid ? '已关联' : '未知' }}</span>
      </div>
    </MdCard>

    <MdCard title="真实网络接口">
      <p v-if="device.error" role="alert">{{ device.error }}</p>
      <p v-for="item in device.snapshot?.network.interfaces" :key="item.index">{{ item.name }} · {{ item.ipv4 ?? '未分配 IPv4' }} · {{ item.up === true ? '已启用' : item.up === false ? '未启用' : '状态未知' }}</p>
      <p v-if="!device.snapshot?.network.interfaces.length" class="muted">{{ device.snapshot?.network.available ? '未发现接口' : '接口枚举未提供' }}</p>
      <MdButton :disabled="!device.available || device.busy" @click="device.refresh()">刷新接口</MdButton>
    </MdCard>

    <MdCard title="WiFi 配置">
      <Skeleton v-if="setup.statusBusy.value && !net" :lines="2" />
      <EmptyState v-else-if="!net" tone="error" compact title="读取网络状态失败" :hint="setup.statusError.value ?? undefined" action-text="重试" @action="setup.refreshStatus()" />
      <template v-else>
        <dl class="kv">
          <div><dt>状态</dt><dd>{{ net.state ? NETWORK_STATE_LABEL[net.state] : '未知' }}</dd></div>
          <div><dt>已保存的 WiFi</dt><dd>{{ net.ssid ?? '无' }}</dd></div>
          <div v-if="net.ipv4"><dt>地址</dt><dd class="mono">{{ net.ipv4 }}</dd></div>
          <div v-if="net.hostname"><dt>路由器显示名</dt><dd class="mono">{{ net.hostname }}</dd></div>
          <div v-if="net.error"><dt>最近的错误</dt><dd>{{ net.error }}</dd></div>
        </dl>
        <div class="row" style="justify-content: flex-end; margin-top: 12px">
          <MdButton variant="text" :disabled="setup.statusBusy.value" @click="setup.refreshStatus()">刷新</MdButton>
          <MdButton variant="outlined" :disabled="!net.configured || !setup.session.isOwner" @click="forgetWifi()">清除保存的 WiFi</MdButton>
        </div>
      </template>
    </MdCard>

    <MdCard title="切换网络">
      <template v-if="setup.phase.value === 'joining'">
        <WifiJoinNotice :ssid="setup.joinSsid.value" :via-hotspot="setup.viaHotspot.value" :link-dropped="setup.linkDropped.value" />
        <div class="row" style="justify-content: flex-end; margin-top: 8px"><MdButton variant="text" :disabled="!setup.session.connected" @click="setup.retry()">重新填写</MdButton></div>
      </template>
      <EmptyState v-else-if="setup.phase.value === 'joined'" icon="check_circle" compact title="设备已连上 WiFi" :hint="`已加入「${setup.joinSsid.value}」`" action-text="好的" @action="setup.retry()" />
      <template v-else>
        <p class="muted" style="font-size: 13px; margin: 0 0 12px">为设备配置新的 WiFi。切换后设备的地址可能变化，新地址会显示在它的眼睛屏幕上。</p>
        <p v-if="setup.joinError.value" class="join-error" role="alert">{{ setup.joinError.value }}</p>
        <WifiSetupForm :setup="setup" />
      </template>
    </MdCard>
  </div>
</template>

<style scoped>
.wifi { display: flex; align-items: center; gap: 14px; }
.orb {
  width: 48px;
  height: 48px;
  border-radius: 50%;
  display: grid;
  place-items: center;
  background: rgba(var(--md-primary-rgb), 0.14);
  color: var(--md-primary);
  flex: none;
}
.wifi-body { flex: 1; min-width: 0; }
.ssid { font: 600 16px var(--font-title); color: var(--md-on-surface); }
.sub { font-size: 12.5px; margin-top: 2px; display: flex; align-items: center; flex-wrap: wrap; gap: 6px; }
.bars { display: inline-flex; align-items: flex-end; gap: 2px; height: 12px; flex: none; }
.bars i { width: 3px; background: var(--md-outline-variant); border-radius: 1px; }
.bars i:nth-child(1) { height: 4px; }
.bars i:nth-child(2) { height: 7px; }
.bars i:nth-child(3) { height: 10px; }
.bars i:nth-child(4) { height: 12px; }
.bars i.on { background: var(--md-primary); }
.join-error { margin: 0 0 12px; font-size: 13px; color: var(--md-error); }
.md-btn :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
</style>
