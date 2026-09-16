<script setup lang="ts">
/* Sleep mini: sleep switch (eye mode 'sleep' / 'idle') + brightness slider.
 * Shares nyabula.feature.sleep with SleepFeature; the slider re-pushes
 * { asleep, wake_by, brightness } on release while the scene is active. */
import { computed, reactive, ref, watch } from 'vue';
import { MdSlider, MdSwitch, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory, type FeatureMiniProps } from './contract';

const props = defineProps<FeatureMiniProps>();
const eye = useEyeStore();

const state = reactive(useFeatureMemory(props.type, { wakeBy: ['voice', 'touch'] as string[], brightness: 70, dimOnSleep: true }));
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const asleep = computed(() => eye.activeMode === 'sleep');
const brightness = ref(state.brightness);
watch(() => props.payload, (pl) => {
  if (props.active && pl && typeof pl.brightness === 'number') brightness.value = pl.brightness;
}, { immediate: true });

function setAsleep(v: boolean): void {
  void eye.setMode(v ? 'sleep' : 'idle');
}
function commit(): void {
  state.brightness = brightness.value;
  if (props.active) void eye.setScene(props.type, eye.sceneStyle, { asleep: asleep.value, wake_by: [...state.wakeBy], brightness: state.brightness });
}
</script>

<template>
  <div class="slm">
    <label class="slm-sw">
      <UiIcon name="moon" :size="18" :class="{ on: asleep }" />
      <span>{{ asleep ? '休眠中' : '休眠' }}</span>
      <MdSwitch :model-value="asleep" @update:model-value="setAsleep" />
    </label>
    <div class="slm-bright">
      <UiIcon name="light_mode" :size="16" class="ic" />
      <MdSlider v-model="brightness" :min="0" :max="100" aria-label="亮度" @change="commit" />
      <span class="pct">{{ brightness }}%</span>
    </div>
  </div>
</template>

<style scoped>
.slm { display: flex; align-items: center; gap: 12px; min-height: 44px; }
.slm-sw { display: flex; align-items: center; gap: 6px; min-height: 40px; flex: none; font: 600 13px var(--font-body); color: var(--md-on-surface); cursor: pointer; }
.slm-sw .on { color: var(--md-primary); }
.slm-bright { flex: 1; min-width: 0; display: flex; align-items: center; gap: 8px; }
.ic { color: var(--md-on-surface-variant); flex: none; }
.slm-bright :deep(.md-slider) { flex: 1; min-width: 50px; }
.pct { font-size: 12px; color: var(--md-on-surface-variant); min-width: 34px; text-align: right; font-variant-numeric: tabular-nums; }
</style>
