<script setup lang="ts">
/* Home mini: four scene chips (tap = apply + push) + lights switch. Shares
 * nyabula.feature.home with HomeFeature and mirrors its applyScene rules. */
import { computed, reactive, watch } from 'vue';
import { MdSwitch, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory, type FeatureMiniProps } from './contract';

const props = defineProps<FeatureMiniProps>();
const eye = useEyeStore();

const SCENES = [
  { id: 'home', label: '回家', icon: 'home' },
  { id: 'away', label: '离家', icon: 'logout' },
  { id: 'sleep', label: '睡眠', icon: 'moon' },
  { id: 'cinema', label: '影院', icon: 'play_arrow' },
];

const state = reactive(useFeatureMemory(props.type, { scene: 'home', lights: true, brightness: 80, curtain: true, ac: false, temp: 24 }));
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const scene = computed(() => (props.active && typeof props.payload?.scene === 'string' ? props.payload.scene : state.scene));
const lights = computed(() => (props.active && typeof props.payload?.lights === 'boolean' ? props.payload.lights : state.lights));

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { scene: state.scene, lights: state.lights, temp: state.ac ? state.temp : null });
}
function applyScene(id: string): void {
  state.scene = id;
  if (id === 'home') { state.lights = true; state.brightness = 80; state.curtain = true; }
  else if (id === 'away') { state.lights = false; state.ac = false; }
  else if (id === 'sleep') { state.lights = false; state.curtain = false; state.ac = true; state.temp = 26; }
  else if (id === 'cinema') { state.lights = true; state.brightness = 20; state.curtain = false; }
  push();
}
function setLights(v: boolean): void {
  state.lights = v;
  if (props.active) push();
}
</script>

<template>
  <div class="hom">
    <div class="hom-scenes">
      <button v-for="s in SCENES" :key="s.id" type="button" class="hom-chip" :class="{ on: scene === s.id }" @click="applyScene(s.id)">
        <UiIcon :name="s.icon" :size="16" /><span>{{ s.label }}</span>
      </button>
    </div>
    <label class="hom-sw">
      <UiIcon name="lightbulb" :size="16" :class="{ on: lights }" />
      <MdSwitch :model-value="lights" @update:model-value="setLights" />
    </label>
  </div>
</template>

<style scoped>
.hom { display: flex; align-items: center; gap: 8px; min-height: 44px; }
.hom-scenes { flex: 1; min-width: 0; display: flex; gap: 4px; overflow-x: auto; scrollbar-width: none; }
.hom-scenes::-webkit-scrollbar { display: none; }
.hom-chip {
  flex: none; display: inline-flex; align-items: center; gap: 4px; min-height: 40px; padding: 0 10px;
  border: 1px solid var(--md-outline-variant); border-radius: var(--radius-s); background: transparent;
  color: var(--md-on-surface-variant); font: 600 12.5px var(--font-body); cursor: pointer;
  transition: background var(--dur-fast), color var(--dur-fast), border-color var(--dur-fast);
}
.hom-chip:hover { border-color: var(--md-outline); color: var(--md-on-surface); }
.hom-chip.on { background: var(--md-secondary-container); color: var(--md-on-secondary-container); border-color: transparent; }
.hom-sw { display: flex; align-items: center; gap: 6px; min-height: 40px; flex: none; color: var(--md-on-surface-variant); cursor: pointer; }
.hom-sw .on { color: var(--md-primary); }
</style>
