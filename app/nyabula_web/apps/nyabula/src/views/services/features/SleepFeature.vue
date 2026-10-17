<script setup lang="ts">
/* Sleep feature. Sleeping is an eye expression on the device ('sleep'); waking
 * releases it so the agent's own expressions show again. Wake sources and
 * screen dimming need driver work and are listed as planned, not as switches. */
import { computed } from 'vue';
import { NkListSection, NkRow, NkToggleRow, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { MODE_LABELS, useEyeStore } from '../../../stores/eye';
import { useSessionStore } from '../../../stores/session';
import type { FormFactor } from '../../../composables/useFormFactor';

defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();
const session = useSessionStore();

const PLANNED = [
  { icon: 'mic', title: '语音唤醒', sub: '唤醒词“你好，openvela”，待语音前端接入' },
  { icon: 'toggle_on', title: '触摸唤醒', sub: '待触摸传感器驱动接入' },
  { icon: 'dark_mode', title: '休眠时调暗屏幕', sub: '待背光控制接入' },
];

const asleep = computed(() => eye.activeMode === 'sleep');
const subtitle = computed(() => (!session.connected ? '设备离线' : asleep.value ? '休眠中' : `清醒 · ${MODE_LABELS[eye.activeMode] ?? eye.activeMode}`));

function setAsleep(v: boolean): void {
  void eye.setMode(v ? 'sleep' : 'idle');
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
            <div class="muted">{{ asleep ? '眼睛已闭上，可在这里唤醒' : '切换后眼睛会慢慢闭上' }}</div>
          </div>
        </div>
        <NkListSection>
          <NkToggleRow :model-value="asleep" icon="moon" title="休眠" :sub="asleep ? '点按唤醒' : '点按进入休眠'" :disabled="!session.canControl" @update:model-value="setAsleep" />
        </NkListSection>
        <p class="muted small">休眠是设备上的眼睛表情，关闭网页后保持；唤醒会把表情交还给猫猫自己。睡眠定时到点也会进入这里的休眠。</p>
      </section>
      <section class="card">
        <NkListSection title="规划中" :card="false">
          <NkRow v-for="p in PLANNED" :key="p.title" :icon="p.icon" :title="p.title" :sub="p.sub"><span class="tag">规划中</span></NkRow>
        </NkListSection>
      </section>
    </div>
  </div>
</template>

<style scoped>
.tag { font: 600 11px var(--font-body); padding: 2px 8px; border-radius: 999px; background: var(--md-surface-container-highest); color: var(--md-on-surface-variant); }
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
