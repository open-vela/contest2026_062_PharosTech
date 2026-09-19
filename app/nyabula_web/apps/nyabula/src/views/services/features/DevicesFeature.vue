<script setup lang="ts">
/* Hardware attached to the device, as Core enumerates it in device.status:
 * audio devices, storage volumes and network interfaces. Nothing is invented
 * and nothing is kept in the browser; discovery of nearby gadgets (Bluetooth,
 * home devices) has no device topic yet and is not offered. */
import { computed } from 'vue';
import { NkListSection, NkRow, NkStatTile, MdButton, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { formatDeviceBytes, useDeviceRuntime } from '../../../composables/useDeviceRuntime';
import { hardwareCounts } from '../../../composables/eyeScenePayload';
import { useDevicesScene } from './sceneLinks';
import type { FormFactor } from '../../../composables/useFormFactor';
import EyeShowButton from './EyeShowButton.vue';

const props = defineProps<{ type: string; ff: FormFactor }>();
const device = useDeviceRuntime();
const scene = useDevicesScene(props.type, device);
const active = computed(() => scene.shown.value || scene.held.value);

interface Row { id: string; icon: string; name: string; sub: string; online: boolean }
const rows = computed<Row[]>(() => {
  const s = device.snapshot;
  if (!s) return [];
  return [
    ...s.audioDevices.map((a) => ({ id: 'a' + a.path, icon: a.input && !a.output ? 'mic' : 'volume_up', name: a.path, online: a.available,
      sub: a.available ? [a.output ? '输出' : '', a.input ? '输入' : ''].filter(Boolean).join(' / ') || '音频设备' : '音频设备 · 不可用' })),
    ...s.storage.map((v) => ({ id: 's' + v.path, icon: 'storage', name: v.path, online: v.available,
      sub: v.available ? `存储 · 可用 ${formatDeviceBytes(v.freeBytes)}` : '存储 · 未挂载' })),
    ...s.network.interfaces.filter((i) => !i.loopback).map((i) => ({ id: 'n' + i.index, icon: i.ssid ? 'wifi' : 'link', name: i.name, online: i.up === true,
      sub: i.up === true ? `网络 · ${i.ssid ?? i.ipv4 ?? '已启用'}` : '网络 · 未启用' })),
  ];
});
const counts = computed(() => hardwareCounts(device.snapshot));
const subtitle = computed(() => !device.available ? '需要以主人身份连接支持诊断的 Core'
  : device.snapshot ? `${counts.value.total} 在线 · 共 ${rows.value.length} 个` : '读取中');
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="devices" title="设备" :subtitle="subtitle" :tone="active ? 'ok' : 'default'">
      <MdButton variant="tonal" :disabled="device.busy || !device.available" @click="device.refresh()"><UiIcon name="refresh" :size="20" />刷新</MdButton>
    </NkHeader>
    <p v-if="device.error" role="alert">{{ device.error }}</p>
    <p v-if="device.snapshot?.simulator" class="muted small">当前为模拟器枚举结果，不代表 K7 板上硬件。</p>
    <div class="stats">
      <NkStatTile :value="counts.audio" unit="个" label="音频" icon="volume_up" />
      <NkStatTile :value="counts.storage" unit="个" label="存储" icon="storage" />
      <NkStatTile :value="counts.network" unit="个" label="网络" icon="wifi" />
    </div>
    <section class="card">
      <NkListSection title="已接入硬件" :card="false">
        <NkRow v-for="d in rows" :key="d.id" :icon="d.icon" :title="d.name" :sub="d.sub">
          <span class="state" :class="{ on: d.online }">{{ d.online ? '在线' : '离线' }}</span>
        </NkRow>
      </NkListSection>
      <p v-if="!rows.length" class="muted small">{{ device.available ? '设备尚未返回硬件列表' : '未连接' }}</p>
      <p class="muted small">列表来自设备实时枚举。周边蓝牙 / 家居设备的发现尚无设备接口，暂不提供。</p>
      <EyeShowButton kind="wide" :shown="active" :disabled="!scene.canShow.value" @toggle="scene.toggle()" />
    </section>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.card {
  display: flex; flex-direction: column; gap: 16px;
  padding: 16px; border-radius: var(--radius-l);
  background: var(--md-surface-container); color: var(--md-on-surface);
}
.stats { display: flex; gap: 10px; }
.tiles { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; }
.feature.tablet .tiles { grid-template-columns: repeat(3, 1fr); }
.feature.desktop .tiles { grid-template-columns: repeat(4, 1fr); }
.small { font-size: 12px; margin: 0; }
.state { font-size: 12.5px; color: var(--md-on-surface-variant); }
.state.on { color: var(--md-primary); font-weight: 600; }
</style>
