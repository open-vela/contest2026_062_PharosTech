<script setup lang="ts">
/* Smart home feature. Scene tiles (home / away / sleep / cinema) and
 * light / curtain / AC controls are local demo state; MCP linkage is reserved
 * by contract. Pushes { scene, lights, temp } to the eye. */
import { computed, reactive, watch } from 'vue';
import { NkActionBar, NkListSection, NkSliderRow, NkTile, NkToggleRow } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();

const SCENES = [
  { id: 'home', label: '回家', icon: 'home', sub: '开灯、拉开窗帘' },
  { id: 'away', label: '离家', icon: 'logout', sub: '关灯、关空调' },
  { id: 'sleep', label: '睡眠', icon: 'moon', sub: '关灯、拉上窗帘' },
  { id: 'cinema', label: '影院', icon: 'play_arrow', sub: '调暗灯光' },
];

const mem = useFeatureMemory(props.type, { scene: 'home', lights: true, brightness: 80, curtain: true, ac: false, temp: 24 });
const state = reactive(mem);
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const active = computed(() => eye.activeScene === props.type);
const sceneLabel = computed(() => SCENES.find((s) => s.id === state.scene)?.label ?? '—');
const subtitle = computed(() => `${sceneLabel.value} · 灯光${state.lights ? '开' : '关'} · ${state.ac ? `空调 ${state.temp}°` : '空调关'}`);

function applyScene(id: string): void {
  state.scene = id;
  if (id === 'home') { state.lights = true; state.brightness = 80; state.curtain = true; }
  else if (id === 'away') { state.lights = false; state.ac = false; }
  else if (id === 'sleep') { state.lights = false; state.curtain = false; state.ac = true; state.temp = 26; }
  else if (id === 'cinema') { state.lights = true; state.brightness = 20; state.curtain = false; }
}
function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { scene: state.scene, lights: state.lights, temp: state.ac ? state.temp : null });
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="home" title="家居" :subtitle="subtitle" :tone="active ? 'ok' : 'default'" />
    <div class="grid">
      <section class="card">
        <div class="row between">
          <h3 class="section-title">场景</h3>
          <span class="contract-only">MCP 联动为契约预留</span>
        </div>
        <div class="tiles">
          <NkTile v-for="s in SCENES" :key="s.id" :icon="s.icon" :title="s.label" :sub="s.sub" :active="state.scene === s.id" @tap="applyScene(s.id)" />
        </div>
        <NkActionBar primary-text="显示到眼睛" primary-icon="visibility" secondary-text="隐藏" secondary-icon="close" @primary="push" @secondary="hide" />
      </section>
      <section class="card">
        <NkListSection title="灯光">
          <NkToggleRow v-model="state.lights" icon="lightbulb" title="客厅灯" :sub="state.lights ? `亮度 ${state.brightness}%` : '已关闭'" />
          <NkSliderRow v-model="state.brightness" title="亮度" unit="%" :min="1" :max="100" icon-start="dark_mode" icon-end="light_mode" :disabled="!state.lights" />
        </NkListSection>
        <NkListSection title="窗帘与空调">
          <NkToggleRow v-model="state.curtain" icon="widgets" title="窗帘" :sub="state.curtain ? '已打开' : '已关闭'" />
          <NkToggleRow v-model="state.ac" icon="bolt" title="空调" :sub="state.ac ? `${state.temp}°` : '已关闭'" />
          <NkSliderRow v-model="state.temp" title="温度" unit="°" :min="16" :max="30" :disabled="!state.ac" />
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
.tiles { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; }
.feature.tablet .tiles { grid-template-columns: repeat(4, 1fr); }
</style>
