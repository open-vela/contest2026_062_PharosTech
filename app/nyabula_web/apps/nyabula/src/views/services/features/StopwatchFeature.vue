<script setup lang="ts">
/* Core owns elapsed time and laps; the browser only interpolates the view. */
import { computed } from 'vue';
import { MdButton, NkActionBar, NkChipSelect, NkListSection, UiIcon, useDialogStore } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useCoreTimer } from '../../../composables/useCoreTimer';
import type { FormFactor } from '../../../composables/useFormFactor';

defineProps<{ type: string; ff: FormFactor }>();
const timer = useCoreTimer('stopwatch');
const dialog = useDialogStore();
const instances = computed(() => {
  const rows = timer.choices.value.filter(choice => choice.id !== 'new');
  return rows.map((row, index) => ({ ...row,
    label: rows.filter(other => other.label === row.label).length > 1 ? `${row.label} ${index + 1}` : row.label,
  }));
});
const running = timer.running;
const elapsed = timer.elapsed;
const laps = computed(() => (timer.current.value?.laps ?? []).map((at, i, all) =>
  ({ n: i + 1, at, split: at - (all[i - 1] ?? 0) })).reverse());
const pad = (n: number, w = 2): string => String(n).padStart(w, '0');
function fmt(ms: number): string {
  const cs = Math.floor((ms % 1000) / 10);
  const s = Math.floor(ms / 1000) % 60;
  const m = Math.floor(ms / 60000) % 60;
  const h = Math.floor(ms / 3600000);
  return (h ? pad(h) + ':' : '') + pad(m) + ':' + pad(s) + '.' + pad(cs);
}
const display = computed(() => fmt(elapsed.value));
const subtitle = computed(() => !timer.core.available.value ? '未连接支持计时服务的 Core' :
  running.value ? '设备计时中' : timer.current.value ? '已暂停' : '准备就绪');
const bestLap = computed(() => laps.value.length > 1 ? Math.min(...laps.value.map(l => l.split)) : -1);
const worstLap = computed(() => laps.value.length > 1 ? Math.max(...laps.value.map(l => l.split)) : -1);
async function start() {
  if (timer.current.value) await timer.action('resume');
  else await timer.create(0, '秒表');
}
async function stop() { await timer.action('pause'); }
async function lap() { await timer.action('lap'); }
async function reset() {
  if (!timer.current.value) return;
  if (await dialog.confirm('当前秒表的计时与分段记录将被清除，其他秒表不受影响。',
    { title: '重置秒表', danger: true, confirmText: '重置' })) await timer.action('delete');
}
</script>
<template>
  <div class="feature" :class="ff">
    <NkHeader icon="schedule" title="秒表" :subtitle="subtitle" :tone="running ? 'ok' : 'default'" />
    <div v-if="instances.length" class="instance-row">
      <NkChipSelect v-if="instances.length > 1" :model-value="timer.selected.value" :options="instances" label="当前秒表" @update:model-value="timer.select" />
      <MdButton variant="text" :disabled="timer.core.busy.value || !timer.core.available.value" @click="timer.select('new')">新建秒表</MdButton>
    </div>
    <p v-if="timer.core.error.value" role="alert" class="muted">{{ timer.core.error.value }}</p>
    <div class="grid">
      <section class="card clock-card">
        <div class="clock" :class="{ live: running }">
          <span class="big mono">{{ display }}</span>
          <span class="muted">{{ running ? `第 ${laps.length + 1} 段进行中` : laps.length ? `已记录 ${laps.length} 段` : '按开始计时' }}</span>
        </div>
        <NkActionBar
          :primary-text="running ? '暂停' : elapsed ? '继续' : '开始'"
          :primary-icon="running ? 'pause' : 'play_arrow'"
          :secondary-text="running ? '分段' : '重置'"
          :secondary-icon="running ? 'add' : 'refresh'"
          :danger="running"
          :disabled="timer.core.busy.value || !timer.core.available.value"
          @primary="running ? stop() : start()"
          @secondary="running ? lap() : reset()"
        />
      </section>
      <section class="card laps-card">
        <NkListSection title="分段" :card="false">
          <p v-if="!laps.length" class="muted empty">计时中点「分段」记录一圈</p>
          <div v-for="l in laps" :key="l.n" class="lap-row">
            <span class="lap-n">{{ pad(l.n) }}</span>
            <span class="lap-split mono" :class="{ best: l.split === bestLap, worst: l.split === worstLap }">
              <UiIcon v-if="l.split === bestLap" name="trending_down" :size="16" />
              <UiIcon v-else-if="l.split === worstLap" name="trending_up" :size="16" />
              {{ fmt(l.split) }}
            </span>
            <span class="lap-at mono muted">{{ fmt(l.at) }}</span>
          </div>
        </NkListSection>
      </section>
    </div>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.instance-row { display: flex; justify-content: space-between; align-items: center; gap: 12px; flex-wrap: wrap; }
.grid { display: grid; grid-template-columns: 1fr; gap: 16px; }
.feature.desktop .grid { grid-template-columns: 1fr 1fr; }
.card {
  display: flex; flex-direction: column; gap: 16px;
  padding: 16px; border-radius: var(--radius-l);
  background: var(--md-surface-container); color: var(--md-on-surface);
}
.clock-card { align-items: center; justify-content: center; }
.clock { display: flex; flex-direction: column; align-items: center; gap: 6px; padding: 24px 0; }
.big { font-size: 52px; font-weight: 700; letter-spacing: 1px; font-variant-numeric: tabular-nums; }
.feature.phone .big { font-size: 44px; }
.clock.live .big { color: var(--md-primary); }
.empty { padding: 12px 0; text-align: center; }
.lap-row {
  display: grid; grid-template-columns: 40px 1fr auto; align-items: center; gap: 12px;
  min-height: 44px; padding: 0 4px; border-bottom: 1px solid var(--md-outline-variant);
}
.lap-row:last-child { border-bottom: none; }
.lap-n { color: var(--md-on-surface-variant); font-weight: 600; }
.lap-split { display: inline-flex; align-items: center; gap: 4px; font-variant-numeric: tabular-nums; }
.lap-split.best { color: var(--md-primary); }
.lap-split.worst { color: var(--md-error); }
.lap-at { font-variant-numeric: tabular-nums; }
</style>
