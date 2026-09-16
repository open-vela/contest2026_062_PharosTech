<script setup lang="ts">
/* Health feature. Heart rate is a demo value (rPPG source is reserved by
 * contract) shown on a gauge; steps and sedentary reminder are local stats.
 * Pushes { heart_rate, steps, reminder } to the eye. */
import { computed, reactive, watch } from 'vue';
import { NkActionBar, NkGauge, NkListSection, NkSliderRow, NkStatTile, NkToggleRow } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();

const mem = useFeatureMemory(props.type, { heartRate: 72, steps: 4820, goal: 8000, sitMinutes: 35, reminder: true, sitLimit: 60 });
const state = reactive(mem);
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const active = computed(() => eye.activeScene === props.type);
const zone = computed(() => (state.heartRate < 60 ? '偏低' : state.heartRate <= 100 ? '正常' : state.heartRate <= 140 ? '偏高' : '过高'));
const subtitle = computed(() => `心率 ${state.heartRate} · ${zone.value}`);
const gaugeValue = computed(() => Math.round(((state.heartRate - 40) / 140) * 100));
const stepPct = computed(() => Math.min(100, Math.round((state.steps / state.goal) * 100)));

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { heart_rate: state.heartRate, steps: state.steps, reminder: state.reminder });
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="heart_rate" title="健康" :subtitle="subtitle" :tone="active ? 'ok' : 'default'" />
    <div class="grid">
      <section class="card center">
        <div class="gauge-wrap">
          <NkGauge :value="gaugeValue" :size="ff === 'phone' ? 200 : 240" :stroke="16" label="心率" unit="" :warn-at="72" :error-at="80" />
        </div>
        <div class="hr">
          <span class="big mono">{{ state.heartRate }}</span>
          <span class="muted">次/分 · {{ zone }}</span>
        </div>
        <p class="muted small"><span class="contract-only">rPPG 心率检测为契约预留</span>，下方滑条为演示值。</p>
        <NkSliderRow v-model="state.heartRate" title="演示心率" unit="" :min="40" :max="180" />
        <NkActionBar primary-text="显示到眼睛" primary-icon="visibility" secondary-text="隐藏" secondary-icon="close" @primary="push" @secondary="hide" />
      </section>
      <section class="card">
        <div class="stats">
          <NkStatTile :value="state.steps" unit="步" label="今日步数" icon="trending_up" trend="up" :trend-text="`目标 ${stepPct}%`" />
          <NkStatTile :value="state.sitMinutes" unit="分钟" label="已久坐" icon="schedule" :trend="state.sitMinutes >= state.sitLimit ? 'up' : 'flat'" :trend-text="state.sitMinutes >= state.sitLimit ? '该起来活动了' : '状态良好'" />
        </div>
        <NkListSection title="提醒">
          <NkToggleRow v-model="state.reminder" icon="notifications" title="久坐提醒" sub="到时间眼睛会提醒你起身" />
          <NkSliderRow v-model="state.sitLimit" title="久坐时长" unit="分钟" :min="20" :max="120" :step="5" :disabled="!state.reminder" />
        </NkListSection>
        <NkListSection title="演示数据">
          <NkSliderRow v-model="state.steps" title="步数" unit="步" :min="0" :max="20000" :step="100" />
          <NkSliderRow v-model="state.sitMinutes" title="久坐分钟" unit="分钟" :min="0" :max="180" />
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
.center { align-items: center; }
.center > * { width: 100%; }
.gauge-wrap { display: flex; justify-content: center; }
.hr { display: flex; flex-direction: column; align-items: center; margin-top: -8px; }
.big { font-size: 40px; font-weight: 700; }
.small { font-size: 12px; margin: 0; text-align: center; }
.stats { display: flex; gap: 10px; }
.feature.phone .stats { flex-direction: column; }
</style>
