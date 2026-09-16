<script setup lang="ts">
/* Nearby devices feature. A tile grid of demo peripherals (phone / speaker /
 * earbuds / sensor) with online state; discovery is reserved by contract.
 * Pushes { devices:[{name,type,online}], count } to the eye. */
import { computed, reactive, ref, watch } from 'vue';
import { NkActionBar, NkStatTile, NkTile, MdButton, UiIcon, useToastStore } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();
const toast = useToastStore();

interface Dev { id: string; name: string; type: 'phone' | 'speaker' | 'earbuds' | 'sensor' | 'tablet'; online: boolean }
const TYPE_META: Record<Dev['type'], { icon: string; label: string }> = {
  phone: { icon: 'smartphone', label: '手机' },
  tablet: { icon: 'tablet', label: '平板' },
  speaker: { icon: 'volume_up', label: '音箱' },
  earbuds: { icon: 'bluetooth', label: '耳机' },
  sensor: { icon: 'bolt', label: '传感器' },
};

const mem = useFeatureMemory(props.type, {
  devices: [
    { id: 'd1', name: '我的手机', type: 'phone', online: true },
    { id: 'd2', name: '客厅音箱', type: 'speaker', online: true },
    { id: 'd3', name: '蓝牙耳机', type: 'earbuds', online: false },
    { id: 'd4', name: '温湿度传感器', type: 'sensor', online: true },
  ] as Dev[],
});
const state = reactive(mem);
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const discovering = ref(false);
const onlineCount = computed(() => state.devices.filter((d) => d.online).length);
const active = computed(() => eye.activeScene === props.type);
const subtitle = computed(() => `${onlineCount.value} 在线 · 共 ${state.devices.length} 台`);

function toggle(d: Dev): void {
  d.online = !d.online;
}
function discover(): void {
  if (discovering.value) return;
  discovering.value = true;
  setTimeout(() => {
    discovering.value = false;
    toast.warn('设备发现为契约预留，当前展示演示列表');
  }, 1200);
}
function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, {
    devices: state.devices.map((d) => ({ name: d.name, type: d.type, online: d.online })),
    count: onlineCount.value,
  });
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="devices" title="设备" :subtitle="subtitle" :tone="active ? 'ok' : 'default'">
      <MdButton variant="tonal" :disabled="discovering" @click="discover"><UiIcon :name="discovering ? 'sync' : 'search'" :size="20" />{{ discovering ? '搜索中…' : '发现设备' }}</MdButton>
    </NkHeader>
    <div class="stats">
      <NkStatTile :value="onlineCount" unit="台" label="在线" icon="check_circle" />
      <NkStatTile :value="state.devices.length - onlineCount" unit="台" label="离线" icon="cloud_off" />
    </div>
    <section class="card">
      <div class="row between">
        <h3 class="section-title">周边设备</h3>
        <span class="contract-only">发现为契约预留</span>
      </div>
      <div class="tiles">
        <NkTile
          v-for="d in state.devices"
          :key="d.id"
          :icon="TYPE_META[d.type].icon"
          :title="d.name"
          :sub="TYPE_META[d.type].label + (d.online ? ' · 在线' : ' · 离线')"
          :active="d.online"
          @tap="toggle(d)"
        />
      </div>
      <p class="muted small">点击磁贴可切换演示在线态。</p>
      <NkActionBar primary-text="显示到眼睛" primary-icon="visibility" secondary-text="隐藏" secondary-icon="close" @primary="push" @secondary="hide" />
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
</style>
