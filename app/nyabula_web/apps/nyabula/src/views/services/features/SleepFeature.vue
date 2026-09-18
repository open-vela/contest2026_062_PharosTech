<script setup lang="ts">
/* Sleep feature. The sleep toggle drives the eye mode ('sleep' / 'idle');
 * wake methods and screen brightness are local settings. Pushes
 * { asleep, wake_by, brightness } to the eye. */
import { computed, reactive, watch } from 'vue';
import { NkActionBar, NkChipSelect, NkListSection, NkSliderRow, NkToggleRow, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();

const WAKE = [
  { id: 'voice', label: '语音', icon: 'mic' },
  { id: 'touch', label: '触摸', icon: 'toggle_on' },
  { id: 'schedule', label: '定时', icon: 'alarm' },
];

const mem = useFeatureMemory(props.type, { wakeBy: ['voice', 'touch'] as string[], brightness: 70, dimOnSleep: true });
const state = reactive(mem);
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const asleep = computed(() => eye.activeMode === 'sleep');
const wakeModel = computed({ get: () => state.wakeBy as string | string[], set: (v: string | string[]) => { state.wakeBy = Array.isArray(v) ? v : [v]; } });
const wakeLabels = computed(() => WAKE.filter((w) => state.wakeBy.includes(w.id)).map((w) => w.label).join(' / ') || '无');
const subtitle = computed(() => (asleep.value ? `休眠中 · ${wakeLabels.value}可唤醒` : '清醒'));

function setAsleep(v: boolean): void {
  void eye.setMode(v ? 'sleep' : 'idle');
}
function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { asleep: asleep.value, wake_by: [...state.wakeBy], brightness: state.brightness });
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="moon" title="休眠" :subtitle="subtitle" :tone="asleep ? 'ok' : 'default'" />
    <div class="grid">
      <section class="card">
        <div class="hero" :class="{ asleep }">
          <span class="hero-icon"><UiIcon :name="asleep ? 'moon' : 'light_mode'" :size="40" /></span>
          <div>
            <div class="hero-title">{{ asleep ? '正在休眠' : '清醒中' }}</div>
            <div class="muted">{{ asleep ? '眼睛已闭上，等待唤醒' : '切换后眼睛会慢慢闭上' }}</div>
          </div>
        </div>
        <NkListSection>
          <NkToggleRow :model-value="asleep" icon="moon" title="休眠" :sub="asleep ? '点按唤醒' : '点按进入休眠'" @update:model-value="setAsleep" />
        </NkListSection>
        <NkActionBar primary-text="显示到眼睛" primary-icon="visibility" secondary-text="隐藏" secondary-icon="close" @primary="push" @secondary="hide" />
      </section>
      <section class="card">
        <h3 class="section-title">唤醒方式</h3>
        <NkChipSelect v-model="wakeModel" :options="WAKE" multi />
        <p class="muted small">语音唤醒词：“你好，openvela”。<span class="contract-only">唤醒方式同步为契约预留</span></p>
        <NkListSection title="屏幕">
          <NkSliderRow v-model="state.brightness" title="屏幕亮度" unit="%" :min="5" :max="100" icon-start="dark_mode" icon-end="light_mode" />
          <NkToggleRow v-model="state.dimOnSleep" icon="visibility" title="休眠时调暗" sub="休眠后亮度降到最低" />
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
.hero { display: flex; align-items: center; gap: 14px; padding: 4px; }
.hero-icon {
  display: inline-flex; align-items: center; justify-content: center; width: 72px; height: 72px; flex: none;
  border-radius: var(--radius-l); background: var(--md-surface-container-high); color: var(--md-on-surface-variant);
  transition: background var(--dur-medium) var(--ease-standard), color var(--dur-medium) var(--ease-standard);
}
.hero.asleep .hero-icon { background: var(--md-primary-container); color: var(--md-on-primary-container); }
.hero-title { font-size: 20px; font-weight: 700; }
.small { font-size: 12px; margin: 0; }
</style>
