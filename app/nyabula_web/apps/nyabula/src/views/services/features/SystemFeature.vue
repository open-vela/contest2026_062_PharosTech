<script setup lang="ts">
/* Core owns runtime measurements; unsupported measurements stay unavailable. */
import { computed } from 'vue';
import { NkActionBar, NkGauge, NkKeyValue, NkListSection, NkRow, Skeleton, MdButton, UiIcon, useDialogStore, useToastStore } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useEyeStore } from '../../../stores/eye';
import { useSessionStore } from '../../../stores/session';
import { useAsyncTask } from '../../../composables/useRequest';
import { useDeviceRuntime, formatDeviceBytes } from '../../../composables/useDeviceRuntime';
import { fmtUptime, type SysInfo } from '../../device/sections/sysinfo';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();
const session = useSessionStore();
const dialog = useDialogStore();
const toast = useToastStore();
const device = useDeviceRuntime();

const task = useAsyncTask<SysInfo>(() => session.request('sys.info') as Promise<SysInfo>, { errorPrefix: '读取系统信息失败', immediate: true });
const info = computed(() => task.data.value);

const cpu = computed(() => device.snapshot?.cpu.available ? device.snapshot.cpu.percent ?? null : null);
const memory = computed(() => device.snapshot && device.snapshot.memory.totalBytes > 0 ? 100 * device.snapshot.memory.usedBytes / device.snapshot.memory.totalBytes : null);

const active = computed(() => eye.activeScene === props.type);
const version = computed(() => info.value?.device?.coreVersion ?? '—');
const subtitle = computed(() => (info.value ? `Core ${version.value} · 已运行 ${fmtUptime(info.value.uptime)}` : session.connected ? '读取中' : '设备离线'));

const kv = computed(() => [
  { key: '设备名称', value: info.value?.device?.name ?? session.device?.name ?? '—' },
  { key: '设备 ID', value: info.value?.device?.id ?? '—', mono: true },
  { key: 'Core 版本', value: version.value, mono: true },
  { key: '运行时长', value: fmtUptime(info.value?.uptime) },
  { key: '已用内存', value: formatDeviceBytes(device.snapshot?.memory.usedBytes) },
  { key: '可用内存', value: formatDeviceBytes(device.snapshot?.memory.freeBytes) },
]);

async function reboot(): Promise<void> {
  const ok = await dialog.confirm('设备将重新启动，眼睛会短暂关闭。', { title: '重启设备', danger: true, confirmText: '重启' });
  if (ok) toast.warn('重启为契约预留，设备暂未实现');
}
async function checkUpdate(): Promise<void> {
  const ok = await dialog.confirm('将联系更新服务器检查新版本。', { title: '检查更新', confirmText: '检查' });
  if (ok) toast.warn('更新服务尚未接入，未执行版本检查');
}
function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { uptime: info.value?.uptime ?? 0, version: version.value, ...(cpu.value !== null ? { load: cpu.value / 100 } : {}) });
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="feature system-runtime" :class="ff">
    <NkHeader icon="system" title="系统" :subtitle="subtitle" :tone="active ? 'ok' : 'default'">
      <MdButton variant="icon" aria-label="刷新" :disabled="task.busy.value" @click="task.run()"><UiIcon name="refresh" :size="22" /></MdButton>
    </NkHeader>
    <p v-if="device.error" role="alert">{{ device.error }}</p>
    <p v-if="device.snapshot?.simulator" class="muted">当前为 NuttX 模拟器实时数据，不代表 K7 板上资源。</p>
    <div class="grid">
      <section class="card">
        <div class="gauges">
          <div class="g"><NkGauge v-if="cpu !== null" :value="cpu" label="CPU 采样" :size="ff === 'phone' ? 130 : 150" :warn-at="70" :error-at="90" /><span v-else>CPU 采样未提供</span></div>
          <div class="g"><NkGauge v-if="memory !== null" :value="memory" label="内存" :size="ff === 'phone' ? 130 : 150" :warn-at="75" :error-at="90" /><span v-else>内存数据未提供</span></div>
        </div>
        <p class="muted small">CPU 为调度采样估算；内存来自设备分配器，不再使用演示数值。</p>
        <MdButton variant="text" :disabled="!device.available || device.busy" @click="device.refresh()">刷新设备诊断</MdButton>
        <NkActionBar primary-text="显示到眼睛" primary-icon="visibility" secondary-text="隐藏" secondary-icon="close" @primary="push" @secondary="hide" />
      </section>
      <section class="card">
        <Skeleton v-if="task.busy.value && !info" :lines="4" />
        <NkKeyValue v-else :items="kv" />
        <NkListSection title="存储空间">
          <NkRow v-for="volume in device.snapshot?.storage" :key="volume.path" icon="storage" :title="volume.path" :sub="volume.available ? `可用 ${formatDeviceBytes(volume.freeBytes)} / ${formatDeviceBytes(volume.totalBytes)}` : '未挂载或不可读取'" />
        </NkListSection>
        <NkListSection title="音频设备">
          <NkRow v-for="audio in device.snapshot?.audioDevices" :key="audio.path" icon="volume_up" :title="audio.path" :sub="audio.available ? [audio.output ? '输出' : '', audio.input ? '输入' : ''].filter(Boolean).join(' / ') || '已注册' : '能力查询不可用'" />
          <p v-if="!device.snapshot?.audioDevices.length" class="muted">当前没有已注册音频设备。</p>
        </NkListSection>
        <NkListSection title="维护">
          <NkRow icon="refresh" title="重启设备" sub="约 30 秒后恢复" tappable @tap="reboot"><span class="contract-only">契约预留</span></NkRow>
          <NkRow icon="download" title="检查更新" :sub="`当前 Core ${version}`" tappable @tap="checkUpdate"><span class="contract-only">契约预留</span></NkRow>
        </NkListSection>
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
.gauges { display: flex; justify-content: space-around; gap: 12px; }
.g { display: flex; justify-content: center; }
.small { font-size: 12px; margin: 0; text-align: center; }
</style>
