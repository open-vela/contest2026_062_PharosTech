<script setup lang="ts">
/* Battery mini: level gauge from sys.info + charging icon + "show". Reads
 * the same sys.info as BatteryFeature; shows "—" while disconnected. */
import { computed } from 'vue';
import { MdButton, NkGauge, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useSessionStore } from '../../../stores/session';
import { useAsyncTask } from '../../../composables/useRequest';
import { batteryOf, type SysInfo } from '../../device/sections/sysinfo';
import type { FeatureMiniProps } from './contract';

const props = defineProps<FeatureMiniProps>();
const eye = useEyeStore();
const session = useSessionStore();

const task = useAsyncTask<SysInfo>(() => session.request('sys.info') as Promise<SysInfo>, { immediate: session.connected });
const battery = computed(() => batteryOf(task.data.value));

const level = computed<number | null>(() => {
  if (props.active && typeof props.payload?.level === 'number') return props.payload.level;
  return battery.value.level;
});
const charging = computed(() => (props.active && typeof props.payload?.charging === 'boolean' ? props.payload.charging : battery.value.charging));
const known = computed(() => level.value !== null);
const status = computed(() => (!known.value ? (session.connected ? '读取中' : '设备未连接') : charging.value ? '充电中' : '使用电池'));

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { level: level.value ?? 0, charging: charging.value });
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="btm">
    <div class="btm-gauge">
      <NkGauge :value="level ?? 0" :size="52" :stroke="5" :warn-at="20" :error-at="10" invert unit="" />
      <span class="btm-val" :class="{ dim: !known }">{{ known ? level : '—' }}</span>
    </div>
    <div class="btm-text">
      <span class="btm-line">
        <UiIcon :name="charging ? 'battery_charging' : 'battery'" :size="16" :class="{ chg: charging }" />
        {{ known ? `${level}%` : '—' }}
      </span>
      <span class="btm-sub">{{ status }}</span>
    </div>
    <MdButton v-if="!active" variant="tonal" class="btm-btn" :disabled="!known" @click="push">显示</MdButton>
    <MdButton v-else variant="text" class="btm-btn" @click="hide">隐藏</MdButton>
  </div>
</template>

<style scoped>
.btm { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.btm-gauge { position: relative; flex: none; display: grid; place-items: center; }
.btm-gauge :deep(.nk-gauge-center) { display: none; }
.btm-val { position: absolute; inset: 0; display: grid; place-items: center; font: 700 12px var(--font-body); color: var(--md-on-surface); font-variant-numeric: tabular-nums; }
.btm-val.dim { color: var(--md-on-surface-variant); }
.btm-text { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
.btm-line { display: inline-flex; align-items: center; gap: 4px; font: 600 14px var(--font-body); color: var(--md-on-surface); font-variant-numeric: tabular-nums; }
.btm-line .chg { color: var(--md-primary); }
.btm-sub { font-size: 12px; color: var(--md-on-surface-variant); }
.btm-btn { flex: none; min-height: 40px; padding: 0 16px; }
</style>
