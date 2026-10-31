<script setup lang="ts">
/* The browser interpolates a view; Core owns countdown and completion. */
import { computed, ref, watch } from 'vue';
import { NkActionBar, NkChipSelect, NkDial, NkProgressRing, MdButton, MdTextField } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useCoreTimer } from '../../../composables/useCoreTimer';
import type { FormFactor } from '../../../composables/useFormFactor';

defineProps<{type:string;ff:FormFactor}>();
const timer=useCoreTimer('countdown');
const seconds=ref(300);
const label=ref('倒计时');
const PRESETS=[1,3,5,10,25].map(m=>({label:`${m} 分钟`,seconds:m*60}));
const armed=computed(()=>timer.current.value!==null);
const running=timer.running;
const finished=timer.finished;
const totalMs=computed(()=>timer.current.value?.duration_ms??0);
const remainingMs=timer.remaining;
const progress=computed(()=>totalMs.value?remainingMs.value/totalMs.value:0);
const pad=(n:number)=>String(n).padStart(2,'0');
const display=computed(()=>{
  const s=Math.ceil((armed.value?remainingMs.value:seconds.value*1000)/1000);
  return `${pad(Math.floor(s/60))}:${pad(s%60)}`;
});
const subtitle=computed(()=>!timer.core.available.value?'未连接支持计时服务的 Core':timer.waitingClock.value?'等待设备校准时间':finished.value?'时间到':running.value?'设备计时中':armed.value?'已暂停':'设定时长后开始');
watch(timer.current,t=>{if(t){seconds.value=t.duration_ms/1000;label.value=t.label;}});
async function start(){if(armed.value)await timer.action('resume');else await timer.create(seconds.value*1000,label.value||'倒计时');}
async function pause(){await timer.action('pause');}
async function reset(){await timer.action('delete');}
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="timer" title="倒计时" :subtitle="subtitle" :tone="finished || running ? 'ok' : 'default'" />
    <NkChipSelect :model-value="timer.selected.value" :options="timer.choices.value" label="设备计时器" @update:model-value="timer.select" />
    <p v-if="timer.core.error.value" role="alert" class="muted">{{ timer.core.error.value }}</p>
    <div class="grid">
      <section class="card ring-card">
        <NkProgressRing :value="armed ? progress : 1" :size="ff === 'phone' ? 220 : 260" :stroke="14" :tone="finished ? 'ok' : 'primary'">
          <div class="ring-inner">
            <span class="big mono">{{ display }}</span>
            <span class="muted">{{ label || '倒计时' }}</span>
          </div>
        </NkProgressRing>
        <NkActionBar
          :primary-text="running ? '暂停' : armed ? '继续' : '开始'"
          :primary-icon="running ? 'pause' : 'play_arrow'"
          secondary-text="重置"
          secondary-icon="refresh"
          :disabled="timer.core.busy.value || !timer.core.available.value || timer.waitingClock.value || finished || (!armed && seconds <= 0)"
          @primary="running ? pause() : start()"
          @secondary="reset"
        />
        <MdButton v-if="finished || timer.waitingClock.value" variant="outlined" :disabled="timer.core.busy.value" @click="reset">清除此计时器</MdButton>
      </section>
      <section class="card">
        <h3 class="section-title">设定时长</h3>
        <NkDial v-model="seconds" :presets="PRESETS" :disabled="armed" :min="1" />
        <MdTextField v-model="label" label="标签" placeholder="如：泡茶、番茄钟" :disabled="armed" />
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
