<script setup lang="ts">
/* Network interfaces come from Core; no synthetic scan results. */
import { computed } from 'vue';
import { NkActionBar, NkKeyValue, NkListSection, NkRow, Skeleton, MdButton, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useEyeStore } from '../../../stores/eye';
import { useSessionStore } from '../../../stores/session';
import { useAsyncTask } from '../../../composables/useRequest';
import { useDeviceRuntime } from '../../../composables/useDeviceRuntime';
import { rssiBars, wifiOf, type SysInfo } from '../../device/sections/sysinfo';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();
const session = useSessionStore();
const device = useDeviceRuntime();

const task = useAsyncTask<SysInfo>(() => session.request('sys.info') as Promise<SysInfo>, { errorPrefix: '读取网络信息失败', immediate: true });
const info = computed(() => task.data.value);
const wifi = computed(() => {
  const wireless = device.snapshot?.network.interfaces.find(item => item.ssid);
  return wireless ? { ssid: wireless.ssid ?? null, rssi: null } : wifiOf(info.value);
});
const bars = computed(() => rssiBars(wifi.value.rssi));
const connected = computed(() => session.connected && wifi.value.ssid !== null);

const active = computed(() => eye.activeScene === props.type);
const quality = computed(() => wifi.value.rssi === null ? '信号强度未提供' : ['无信号', '较弱', '一般', '良好', '极佳'][bars.value]);
const subtitle = computed(() => (wifi.value.ssid ? `${wifi.value.ssid} · ${quality.value}` : session.connected ? '未提供 WiFi 关联状态' : '设备离线'));
const TRANSPORT_LABEL: Record<string, string> = { lan: '局域网', cloud: '云端', dev: '演示', self: '设备直连' };

const kv = computed(() => [
  { key: '网络名称', value: wifi.value.ssid ?? '—' },
  { key: '信号强度', value: wifi.value.rssi !== null ? `${wifi.value.rssi} dBm` : '—', mono: true },
  { key: '信号质量', value: quality.value, tone: bars.value >= 3 ? ('ok' as const) : bars.value >= 1 ? ('warn' as const) : ('default' as const) },
  { key: '连接方式', value: session.transport ? TRANSPORT_LABEL[session.transport] ?? session.transport : '—' },
]);

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { ssid: wifi.value.ssid ?? '', rssi: wifi.value.rssi ?? 0, connected: connected.value });
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="feature network-runtime" :class="ff">
    <NkHeader icon="wifi" title="网络" :subtitle="subtitle" :tone="active ? 'ok' : connected ? 'default' : 'warn'">
      <MdButton variant="icon" aria-label="刷新" :disabled="task.busy.value" @click="task.run()"><UiIcon name="refresh" :size="22" /></MdButton>
    </NkHeader>
    <p v-if="device.error" role="alert">{{ device.error }}</p>
    <div class="grid">
      <section class="card">
        <Skeleton v-if="task.busy.value && !info" :lines="4" />
        <template v-else>
          <div class="hero">
            <span class="bars" :class="'b' + bars" aria-hidden="true">
              <i v-for="n in 4" :key="n" :class="{ on: n <= bars }" />
            </span>
            <div class="hero-text">
              <div class="hero-title">{{ wifi.ssid ?? '无线状态未提供' }}</div>
              <div class="muted">{{ connected ? '已关联' : '状态未知' }} · {{ quality }}</div>
            </div>
          </div>
          <NkKeyValue :items="kv" :columns="ff === 'phone' ? 1 : 2" />
        </template>
        <NkActionBar primary-text="显示网络" primary-icon="visibility" secondary-text="隐藏" secondary-icon="close" @primary="push" @secondary="hide" />
      </section>
      <section class="card">
        <NkListSection title="真实网络接口">
          <NkRow v-for="item in device.snapshot?.network.interfaces" :key="item.index" icon="lan" :title="item.name" :sub="`${item.ipv4 ?? '未分配 IPv4'}${item.loopback ? ' · 回环' : ''}`"><span>{{ item.up === true ? '已启用' : item.up === false ? '未启用' : '状态未知' }}</span></NkRow>
          <p v-if="!device.snapshot?.network.interfaces.length" class="muted">{{ device.snapshot?.network.available ? '未发现接口' : '接口枚举未提供' }}</p>
        </NkListSection>
        <MdButton variant="text" :disabled="device.busy || !device.available" @click="device.refresh()">刷新网络接口</MdButton>
        <p class="muted small">接口启用不等于互联网连通。扫描与切换尚未接入，不显示虚构热点。</p>
      </section>
    </div>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.grid { display: grid; grid-template-columns: 1fr; gap: 16px; }
.feature.desktop .grid { grid-template-columns: 1fr 1fr; }
.card {
  display: flex; flex-direction: column; gap: 16px;
  padding: 16px; border-radius: var(--radius-l);
  background: var(--md-surface-container); color: var(--md-on-surface);
}
.hero { display: flex; align-items: center; gap: 16px; padding: 8px 4px; }
.hero-title { font-size: 20px; font-weight: 700; }
.bars { display: inline-flex; align-items: flex-end; gap: 3px; height: 36px; }
.bars i { width: 8px; border-radius: 2px; background: var(--md-surface-container-highest); }
.bars i:nth-child(1) { height: 25%; }
.bars i:nth-child(2) { height: 50%; }
.bars i:nth-child(3) { height: 75%; }
.bars i:nth-child(4) { height: 100%; }
.bars i.on { background: var(--md-primary); }
.bars.sm { height: 20px; }
.bars.sm i { width: 5px; }
.small { font-size: 12px; margin: 0; }
</style>
