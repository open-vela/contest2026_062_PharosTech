<script setup lang="ts">
/* Network mini: SSID + signal bars + dBm + "show". Reads sys.info like
 * NetworkFeature and pushes { ssid, rssi, connected }. */
import { computed } from 'vue';
import { UiIcon } from '@nyabula/ui';
import { useSessionStore } from '../../../stores/session';
import { useDeviceRuntime } from '../../../composables/useDeviceRuntime';
import { useAsyncTask } from '../../../composables/useRequest';
import { parseNetworkStatus, type NetworkStatus } from '../../../lib/wifi';
import { rssiBars, wifiOf, type SysInfo } from '../../device/sections/sysinfo';
import { useNetworkScene } from './sceneLinks';
import type { FeatureMiniProps } from './contract';
import EyeShowButton from './EyeShowButton.vue';

const props = defineProps<FeatureMiniProps>();
const session = useSessionStore();
const device = useDeviceRuntime();

const task = useAsyncTask<SysInfo>(() => session.request('sys.info') as Promise<SysInfo>, { immediate: session.connected });
/* network.status is the only source with a live signal reading; the runtime
 * snapshot names the interface but carries no RSSI, and sys.info reports one
 * only on firmware that puts it there.  Taking the snapshot alone left the
 * card showing a dash on a device that knew its own signal perfectly well.
 */
const status = useAsyncTask<NetworkStatus>(
  async () => parseNetworkStatus(await session.request('network.status')),
  { immediate: session.connected },
);
const wifi = computed(() => {
  const fromInfo = wifiOf(task.data.value);
  const live = status.data.value;
  const wireless = device.snapshot?.network.interfaces.find(item => item.ssid);
  const online = live?.state === 'sta_online';
  return {
    ssid: wireless?.ssid ?? (online ? live?.ssid ?? null : null) ?? fromInfo.ssid,
    rssi: live?.rssi ?? fromInfo.rssi,
  };
});

const ssid = computed<string | null>(() => wifi.value.ssid);
const rssi = computed<number | null>(() => wifi.value.rssi);
const bars = computed(() => rssiBars(rssi.value));
const connected = computed(() => session.connected && wifi.value.ssid !== null);
const label = computed(() => ssid.value ?? (session.connected ? 'WiFi 状态未提供' : '设备离线'));

const scene = useNetworkScene(props.type, () => (session.connected ? { ssid: wifi.value.ssid, rssi: wifi.value.rssi, connected: connected.value } : null));
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
    <EyeShowButton :shown="active || scene.held.value" :disabled="!scene.canShow.value" @toggle="scene.toggle()" />
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
