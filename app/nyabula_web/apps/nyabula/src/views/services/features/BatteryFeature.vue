<script setup lang="ts">
/* Battery feature. Level/charging come from sys.info (real data); the power
 * saving toggle is local (reserved by contract). Pushes { level, charging }. */
import { computed, reactive, watch } from 'vue';
import { NkActionBar, NkBanner, NkGauge, NkKeyValue, NkListSection, NkToggleRow, Skeleton, MdButton, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useEyeStore } from '../../../stores/eye';
import { useSessionStore } from '../../../stores/session';
import { useAsyncTask } from '../../../composables/useRequest';
import { batteryOf, type SysInfo } from '../../device/sections/sysinfo';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();
const session = useSessionStore();

const task = useAsyncTask<SysInfo>(() => session.request('sys.info') as Promise<SysInfo>, { errorPrefix: '读取电量失败', immediate: true });
const info = computed(() => task.data.value);
const battery = computed(() => batteryOf(info.value));
const level = computed(() => battery.value.level ?? 0);
const known = computed(() => battery.value.level !== null);

const mem = useFeatureMemory(props.type, { saver: false, lowAlert: true });
const state = reactive(mem);
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const active = computed(() => eye.activeScene === props.type);
const subtitle = computed(() => (!known.value ? '未读取到电量' : battery.value.charging ? `充电中 · ${level.value}%` : `剩余 ${level.value}%`));
const tone = computed(() => (battery.value.charging ? 'ok' : level.value <= 10 ? 'error' : level.value <= 20 ? 'warn' : 'default'));
const estimate = computed(() => (battery.value.charging ? `约 ${Math.max(5, Math.round((100 - level.value) * 1.2))} 分钟充满（估算）` : `约可用 ${Math.max(0, Math.round(level.value * 0.12))} 小时（估算）`));

const kv = computed(() => [
  { key: '电量', value: known.value ? `${level.value}%` : '—' },
  { key: '状态', value: battery.value.charging ? '充电中' : '使用电池', tone: battery.value.charging ? ('ok' as const) : ('default' as const) },
  { key: '预计', value: known.value ? estimate.value : '—' },
]);

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { level: level.value, charging: battery.value.charging });
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="battery" title="电量" :subtitle="subtitle" :tone="tone">
      <MdButton variant="icon" aria-label="刷新" :disabled="task.busy.value" @click="task.run()"><UiIcon name="refresh" :size="22" /></MdButton>
    </NkHeader>
    <div class="grid">
      <section class="card center">
        <Skeleton v-if="task.busy.value && !info" :lines="4" />
        <template v-else>
          <div class="gauge-wrap">
            <NkGauge :value="level" :size="ff === 'phone' ? 220 : 260" :stroke="18" label="电量" :warn-at="20" :error-at="10" invert />
          </div>
          <div class="status" :class="{ charging: battery.charging }">
            <UiIcon :name="battery.charging ? 'bolt' : 'battery'" :size="20" />
            <span>{{ battery.charging ? '正在充电' : '使用电池中' }}</span>
          </div>
          <NkBanner v-if="known && level <= 20 && !battery.charging" tone="warn" text="电量偏低，请尽快充电。" />
        </template>
        <NkActionBar primary-text="显示电量" primary-icon="visibility" secondary-text="隐藏" secondary-icon="close" :disabled="!known" @primary="push" @secondary="hide" />
      </section>
      <section class="card">
        <NkKeyValue :items="kv" />
        <NkListSection title="电源">
          <NkToggleRow v-model="state.saver" icon="power" title="省电模式" sub="降低屏幕亮度和动画频率" />
          <NkToggleRow v-model="state.lowAlert" icon="notifications" title="低电量提醒" sub="低于 20% 时眼睛提示" />
        </NkListSection>
        <p class="muted small"><span class="contract-only">省电模式与低电量提醒为契约预留</span>，当前仅保存在本机。</p>
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
.status { display: flex; justify-content: center; align-items: center; gap: 6px; color: var(--md-on-surface-variant); font-weight: 600; }
.status.charging { color: var(--md-primary); }
.small { font-size: 12px; margin: 0; }
</style>
