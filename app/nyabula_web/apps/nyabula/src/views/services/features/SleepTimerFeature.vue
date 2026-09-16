<script setup lang="ts">
/* Core owns the sleep countdown. Hardware power actions are not yet available. */
import { computed, ref, watch } from 'vue';
import { NkActionBar, NkChipSelect, NkDial, NkProgressRing, MdButton } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useCoreTimer } from '../../../composables/useCoreTimer';
import type { FormFactor } from '../../../composables/useFormFactor';

defineProps<{ type: string; ff: FormFactor }>();
const timer = useCoreTimer('sleep');
const seconds = ref(1800);
const PRESETS = [15, 30, 45, 60, 90];
const CHIPS = PRESETS.map(m => ({ id: String(m), label: m + ' 分钟' }));
const chip = computed(() => PRESETS.includes(seconds.value / 60) ? String(seconds.value / 60) : '');
function onChip(v: string | string[]) {
  const m = Number(Array.isArray(v) ? v[0] : v);
  if (m) seconds.value = m * 60;
}
const armed = computed(() => timer.current.value !== null);
const running = timer.running;
const expired = timer.finished;
const remainingMs = timer.remaining;
const progress = computed(() => timer.current.value?.duration_ms ? remainingMs.value / timer.current.value.duration_ms : 1);
const pad = (n: number) => String(n).padStart(2, '0');
const display = computed(() => {
  const s = Math.ceil((armed.value ? remainingMs.value : seconds.value * 1000) / 1000);
  const h = Math.floor(s / 3600);
  return (h ? h + ':' : '') + pad(Math.floor((s % 3600) / 60)) + ':' + pad(s % 60);
});
const subtitle = computed(() => !timer.core.available.value ? '未连接支持计时服务的 Core' :
  timer.waitingClock.value ? '等待设备校准时间' : expired.value ? '计时完成，未执行休眠或关屏' :
  running.value ? '设备计时中' : armed.value ? '已暂停' : '设定时长后开始');
watch(timer.current, t => { if (t) seconds.value = t.duration_ms / 1000; });
async function start() {
  if (armed.value) await timer.action('resume');
  else await timer.create(seconds.value * 1000, '睡眠定时');
}
async function pause() { await timer.action('pause'); }
async function cancel() { await timer.action('delete'); }
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="moon" title="睡眠定时" :subtitle="subtitle" :tone="running ? 'ok' : 'default'" />
    <NkChipSelect :model-value="timer.selected.value" :options="timer.choices.value" label="设备睡眠定时器" @update:model-value="timer.select" />
    <p v-if="timer.core.error.value" role="alert" class="muted">{{ timer.core.error.value }}</p>
    <div class="grid">
      <section class="card ring-card">
        <NkProgressRing :value="armed ? progress : 1" :size="ff === 'phone' ? 220 : 260" :stroke="14" :tone="expired ? 'ok' : 'primary'">
          <div class="ring-inner">
            <span class="big mono">{{ display }}</span>
            <span class="muted">{{ running ? '倒计时中' : '睡眠定时' }}</span>
          </div>
        </NkProgressRing>
        <NkActionBar
          :primary-text="running ? '暂停' : armed ? '继续' : '开始'"
          :primary-icon="running ? 'pause' : 'play_arrow'"
          :danger="running"
          :disabled="timer.core.busy.value || !timer.core.available.value || timer.waitingClock.value || expired || (!armed && seconds <= 0)"
          @primary="running ? pause() : start()"
        />
        <MdButton v-if="armed" variant="outlined" :disabled="timer.core.busy.value || !timer.core.available.value" @click="cancel">清除此计时器</MdButton>
      </section>
      <section class="card">
        <h3 class="section-title">时长</h3>
        <NkChipSelect :model-value="chip" :options="CHIPS" :disabled="armed" @update:model-value="onChip" />
        <NkDial v-model="seconds" :min="60" :max="4 * 3600" :step="60" :disabled="armed" />
        <p class="muted">当前仅提供设备端持久计时；休眠和关屏执行尚未接入。</p>
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
.ring-card { align-items: center; }
.ring-inner { display: flex; flex-direction: column; align-items: center; gap: 4px; }
.big { font-size: 44px; font-weight: 700; letter-spacing: 1px; font-variant-numeric: tabular-nums; }
</style>
