<script setup lang="ts">
/* Health mini: heart-rate gauge + steps + sedentary reminder switch. Shares
 * nyabula.feature.health with HealthFeature; the switch re-pushes when active. */
import { computed, reactive, watch } from 'vue';
import { MdSwitch, NkGauge, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory, type FeatureMiniProps } from './contract';

const props = defineProps<FeatureMiniProps>();
const eye = useEyeStore();

const state = reactive(useFeatureMemory(props.type, { heartRate: 72, steps: 4820, goal: 8000, sitMinutes: 35, reminder: true, sitLimit: 60 }));
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const hr = computed(() => (props.active && typeof props.payload?.heart_rate === 'number' ? props.payload.heart_rate : state.heartRate));
const steps = computed(() => (props.active && typeof props.payload?.steps === 'number' ? props.payload.steps : state.steps));
const gaugeValue = computed(() => Math.round(((hr.value - 40) / 140) * 100));

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { heart_rate: state.heartRate, steps: state.steps, reminder: state.reminder });
}
function toggleScene(): void {
  if (props.active) void eye.setScene(null);
  else push();
}
function setReminder(v: boolean): void {
  state.reminder = v;
  if (props.active) push();
}
</script>

<template>
  <div class="hm">
    <button type="button" class="hm-gauge" :title="active ? '隐藏' : '显示'" :aria-label="active ? '隐藏' : '显示'" @click="toggleScene">
      <NkGauge :value="gaugeValue" :size="52" :stroke="5" :warn-at="72" :error-at="100" unit="" />
      <span class="hm-hr">{{ hr }}</span>
    </button>
    <div class="hm-text">
      <span class="hm-line"><UiIcon name="heart_rate" :size="14" /> 心率 {{ hr }} bpm</span>
      <span class="hm-line sub"><UiIcon name="trending_up" :size="14" /> 步数 {{ steps.toLocaleString() }}</span>
    </div>
    <label class="hm-sw">
      <span>久坐提醒</span>
      <MdSwitch :model-value="state.reminder" @update:model-value="setReminder" />
    </label>
  </div>
</template>

<style scoped>
.hm { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.hm-gauge { position: relative; flex: none; border: none; background: transparent; padding: 0; cursor: pointer; display: grid; place-items: center; border-radius: 50%; }
.hm-gauge :deep(.nk-gauge-center) { display: none; }
.hm-hr { position: absolute; inset: 0; display: grid; place-items: center; font: 700 12px var(--font-body); color: var(--md-on-surface); pointer-events: none; font-variant-numeric: tabular-nums; }
.hm-text { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
.hm-line { display: inline-flex; align-items: center; gap: 4px; font: 600 13px var(--font-body); color: var(--md-on-surface); white-space: nowrap; }
.hm-line.sub { font-weight: 400; font-size: 12px; color: var(--md-on-surface-variant); }
.hm-sw { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--md-on-surface-variant); min-height: 40px; flex: none; cursor: pointer; }
</style>
