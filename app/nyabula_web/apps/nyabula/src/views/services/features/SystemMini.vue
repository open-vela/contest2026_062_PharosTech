<script setup lang="ts">
/* System mini: Core version + uptime from sys.info + "show". Pushes the
 * same { uptime, version, load } shape as SystemFeature. */
import { computed } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useSessionStore } from '../../../stores/session';
import { useDeviceRuntime } from '../../../composables/useDeviceRuntime';
import { useAsyncTask } from '../../../composables/useRequest';
import { fmtUptime, type SysInfo } from '../../device/sections/sysinfo';
import type { FeatureMiniProps } from './contract';

const props = defineProps<FeatureMiniProps>();
const eye = useEyeStore();
const session = useSessionStore();
const device = useDeviceRuntime();

const task = useAsyncTask<SysInfo>(() => session.request('sys.info') as Promise<SysInfo>, { immediate: session.connected });
const info = computed(() => task.data.value);

const version = computed(() => (props.active && typeof props.payload?.version === 'string' ? props.payload.version : info.value?.device?.coreVersion ?? '—'));
const uptime = computed(() => (props.active && typeof props.payload?.uptime === 'number' ? props.payload.uptime : info.value?.uptime));
const uptimeText = computed(() => (uptime.value !== undefined ? fmtUptime(uptime.value) : session.connected ? '读取中' : '设备离线'));

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { uptime: info.value?.uptime ?? 0, version: info.value?.device?.coreVersion ?? '—', ...(device.snapshot?.cpu.available && device.snapshot.cpu.percent !== undefined ? { load: device.snapshot.cpu.percent / 100 } : {}) });
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="sm">
    <div class="sm-text">
      <span class="sm-line"><UiIcon name="system" :size="15" /> Core <span class="mono">{{ version }}</span></span>
      <span class="sm-line sub"><UiIcon name="schedule" :size="14" /> 已运行 {{ uptimeText }}</span>
    </div>
    <MdButton v-if="!active" variant="tonal" class="sm-btn" :disabled="!info" @click="push">显示</MdButton>
    <MdButton v-else variant="text" class="sm-btn" @click="hide">隐藏</MdButton>
  </div>
</template>

<style scoped>
.sm { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.sm-text { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 3px; }
.sm-line { display: inline-flex; align-items: center; gap: 5px; font: 600 14px var(--font-body); color: var(--md-on-surface); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.sm-line.sub { font-weight: 400; font-size: 12px; color: var(--md-on-surface-variant); }
.mono { font-family: var(--font-mono, ui-monospace, monospace); font-size: 13px; }
.sm-btn { flex: none; min-height: 40px; padding: 0 16px; }
</style>
