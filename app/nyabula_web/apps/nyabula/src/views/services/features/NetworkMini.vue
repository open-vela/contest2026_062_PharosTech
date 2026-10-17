<script setup lang="ts">
/* Network mini: SSID + signal bars + dBm + "show". Reads sys.info like
 * NetworkFeature and pushes { ssid, rssi, connected }. */
import { computed } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useSessionStore } from '../../../stores/session';
import { useDeviceRuntime } from '../../../composables/useDeviceRuntime';
import { useAsyncTask } from '../../../composables/useRequest';
import { rssiBars, wifiOf, type SysInfo } from '../../device/sections/sysinfo';
import type { FeatureMiniProps } from './contract';

const props = defineProps<FeatureMiniProps>();
const eye = useEyeStore();
const session = useSessionStore();
const device = useDeviceRuntime();

const task = useAsyncTask<SysInfo>(() => session.request('sys.info') as Promise<SysInfo>, { immediate: session.connected });
const wifi = computed(() => {
  const wireless = device.snapshot?.network.interfaces.find(item => item.ssid);
  return wireless ? { ssid: wireless.ssid ?? null, rssi: null } : wifiOf(task.data.value);
});

const ssid = computed<string | null>(() => (props.active && typeof props.payload?.ssid === 'string' && props.payload.ssid ? props.payload.ssid : wifi.value.ssid));
const rssi = computed<number | null>(() => (props.active && typeof props.payload?.rssi === 'number' && props.payload.rssi !== 0 ? props.payload.rssi : wifi.value.rssi));
const bars = computed(() => rssiBars(rssi.value));
const connected = computed(() => session.connected && wifi.value.ssid !== null);
const label = computed(() => ssid.value ?? (session.connected ? 'WiFi 状态未提供' : '设备离线'));

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { ssid: wifi.value.ssid ?? '', rssi: wifi.value.rssi ?? 0, connected: connected.value });
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="nm">
    <UiIcon :name="ssid ? 'wifi' : 'wifi_off'" :size="20" class="nm-ic" :class="{ on: ssid }" />
    <div class="nm-text">
      <span class="nm-ssid">{{ label }}</span>
      <span class="nm-sub">
        <span class="nm-bars" :aria-label="`信号 ${bars}/4`"><i v-for="i in 4" :key="i" :class="{ on: i <= bars }" :style="{ height: 4 + i * 3 + 'px' }" /></span>
        <span class="nm-dbm">{{ rssi !== null ? `${rssi} dBm` : '—' }}</span>
      </span>
    </div>
    <MdButton v-if="!active" variant="tonal" class="nm-btn" @click="push">显示</MdButton>
    <MdButton v-else variant="text" class="nm-btn" @click="hide">隐藏</MdButton>
  </div>
</template>

<style scoped>
.nm { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.nm-ic { flex: none; color: var(--md-on-surface-variant); }
.nm-ic.on { color: var(--md-primary); }
.nm-text { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 3px; }
.nm-ssid { font: 600 14px var(--font-body); color: var(--md-on-surface); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.nm-sub { display: inline-flex; align-items: center; gap: 8px; font-size: 12px; color: var(--md-on-surface-variant); }
.nm-bars { display: inline-flex; align-items: flex-end; gap: 2px; height: 16px; }
.nm-bars i { width: 4px; border-radius: 1px; background: var(--md-outline-variant); }
.nm-bars i.on { background: var(--md-primary); }
.nm-dbm { font-variant-numeric: tabular-nums; }
.nm-btn { flex: none; min-height: 40px; padding: 0 16px; }
</style>
