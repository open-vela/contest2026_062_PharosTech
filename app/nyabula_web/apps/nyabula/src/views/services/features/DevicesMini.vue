<script setup lang="ts">
/* Devices mini: online count + device icons (online highlighted, tap to
 * toggle) + "show". Shares nyabula.feature.devices with DevicesFeature. */
import { computed, reactive, watch } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory, type FeatureMiniProps } from './contract';

const props = defineProps<FeatureMiniProps>();
const eye = useEyeStore();

type DevType = 'phone' | 'speaker' | 'earbuds' | 'sensor' | 'tablet';
interface Dev { id: string; name: string; type: DevType; online: boolean }
const ICON: Record<DevType, string> = { phone: 'smartphone', tablet: 'tablet', speaker: 'volume_up', earbuds: 'bluetooth', sensor: 'bolt' };

const state = reactive(useFeatureMemory(props.type, {
  devices: [
    { id: 'd1', name: '我的手机', type: 'phone', online: true },
    { id: 'd2', name: '客厅音箱', type: 'speaker', online: true },
    { id: 'd3', name: '蓝牙耳机', type: 'earbuds', online: false },
    { id: 'd4', name: '温湿度传感器', type: 'sensor', online: true },
  ] as Dev[],
}));
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const shown = computed(() => state.devices.slice(0, 4));
const onlineCount = computed(() => {
  if (props.active && typeof props.payload?.count === 'number') return props.payload.count;
  return state.devices.filter((d) => d.online).length;
});

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, {
    devices: state.devices.map((d) => ({ name: d.name, type: d.type, online: d.online })),
    count: state.devices.filter((d) => d.online).length,
  });
}
function toggle(d: Dev): void {
  d.online = !d.online;
  if (props.active) push();
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="dm">
    <div class="dm-count"><b>{{ onlineCount }}</b><span>在线</span></div>
    <div class="dm-icons">
      <button v-for="d in shown" :key="d.id" type="button" class="dm-dev" :class="{ on: d.online }" :title="`${d.name}${d.online ? '' : '（离线）'}`" :aria-label="d.name" @click="toggle(d)">
        <UiIcon :name="ICON[d.type]" :size="18" />
      </button>
    </div>
    <MdButton v-if="!active" variant="tonal" class="dm-btn" @click="push">显示</MdButton>
    <MdButton v-else variant="text" class="dm-btn" @click="hide">隐藏</MdButton>
  </div>
</template>

<style scoped>
.dm { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.dm-count { display: flex; align-items: baseline; gap: 4px; flex: none; }
.dm-count b { font: 700 20px var(--font-body); color: var(--md-primary); font-variant-numeric: tabular-nums; }
.dm-count span { font-size: 12px; color: var(--md-on-surface-variant); }
.dm-icons { flex: 1; display: flex; gap: 2px; }
.dm-dev {
  width: 40px; height: 40px; border: none; border-radius: 50%; display: grid; place-items: center; padding: 0; cursor: pointer;
  background: transparent; color: var(--md-outline);
}
.dm-dev:hover { background: var(--md-surface-container-highest); }
.dm-dev.on { background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.dm-btn { flex: none; min-height: 40px; padding: 0 16px; }
</style>
