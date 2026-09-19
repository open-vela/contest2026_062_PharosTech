<script setup lang="ts">
import { computed, onMounted, reactive, watch } from 'vue';
import { MdButton, NkChipSelect, NkListSection, NkSegmentRow, NkSliderRow, NkToggleRow, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useCompanionStore } from '../../../stores/companion';
import type { FormFactor } from '../../../composables/useFormFactor';
import { useVisiblePoll } from '../../../composables/usePageVisible';
import { useCompanionScene } from './sceneLinks';
import EyeShowButton from './EyeShowButton.vue';

const props = defineProps<{ type:string; ff:FormFactor }>();
const companion = useCompanionStore();
const form = reactive({ enabled:false, mode:'quiet' as 'quiet'|'interactive'|'story', quiet_start:1320,
  quiet_end:480, utc_offset_minutes:-new Date().getTimezoneOffset(), minimum_interval_minutes:180, daily_limit:3 });
const MODES = [
  { id:'quiet', label:'安静陪伴', icon:'pets', blurb:'偶尔轻声问候，不连续追问' },
  { id:'interactive', label:'适度互动', icon:'auto_awesome', blurb:'结合可靠的日程、天气或已保存记忆' },
  { id:'story', label:'睡前陪伴', icon:'article', blurb:'只发一段简短、温和的文字' },
];
let syncing = false;
onMounted(async () => { await companion.refresh(); sync(); });
useVisiblePoll(() => void companion.refresh(), 5000);
const scene = useCompanionScene(props.type);
const onEyes = computed(() => scene.shown.value || scene.held.value);
watch(() => companion.state, sync);
function sync():void {
  if (!companion.state || syncing) return;
  Object.assign(form, companion.state);
}
let saveTimer:ReturnType<typeof setTimeout>|undefined;
watch(form, () => {
  if (!companion.state) return;
  clearTimeout(saveTimer);
  saveTimer=setTimeout(async () => { syncing=true; await companion.act('configure', { ...form }); syncing=false; }, 350);
}, { deep:true });
const modeDef=computed(() => MODES.find(item=>item.id===form.mode) ?? MODES[0]);
function fmtMinute(value:number):string { return `${String(Math.floor(value/60)).padStart(2,'0')}:${String(value%60).padStart(2,'0')}`; }
const subtitle=computed(() => !companion.available ? '请连接支持主动陪伴的 Core 设备'
  : !form.enabled ? '已关闭' : companion.state?.quiet_now ? '免打扰中'
  : companion.state?.pending_run ? '正在准备一条问候'
  : `今日 ${companion.state?.daily_count ?? 0}/${form.daily_limit} 次`);
const quietStartModel=computed({ get:()=>String(form.quiet_start), set:(value:string)=>{ form.quiet_start=Number(value); } });
const quietEndModel=computed({ get:()=>String(form.quiet_end), set:(value:string)=>{ form.quiet_end=Number(value); } });
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="companion" title="主动陪伴" :subtitle="subtitle" :tone="form.enabled && !companion.state?.quiet_now ? 'ok' : 'default'" />
    <p v-if="companion.error" role="alert">{{ companion.error }}</p>
    <div class="grid">
      <section class="card hero" :class="{ on:form.enabled }">
        <div class="hero-icon"><UiIcon :name="modeDef.icon" :size="40" /></div>
        <h2>{{ modeDef.label }}</h2><p>{{ modeDef.blurb }}</p>
        <NkToggleRow v-model="form.enabled" title="允许主动陪伴" sub="默认关闭；打开后规则保存在设备端" />
        <MdButton variant="tonal" :disabled="!form.enabled || companion.state?.quiet_now || companion.busy" @click="companion.act('run', {})">现在问候一次</MdButton>
        <EyeShowButton kind="wide" :shown="onEyes" :disabled="!scene.canShow.value" @toggle="scene.toggle()" />
      </section>
      <section class="card">
        <NkListSection title="怎么陪" :card="false">
          <NkChipSelect v-model="form.mode" :options="MODES" />
          <NkSliderRow v-model="form.minimum_interval_minutes" title="最短互动间隔" :min="30" :max="720" :step="30" unit=" 分钟" />
          <NkSliderRow v-model="form.daily_limit" title="每天最多主动次数" :min="1" :max="8" :step="1" unit=" 次" />
          <NkSegmentRow v-model="quietStartModel" title="免打扰开始" :items="[
            {id:'1200',label:'20:00'},{id:'1320',label:'22:00'},{id:'1380',label:'23:00'}]" />
          <NkSegmentRow v-model="quietEndModel" title="免打扰结束" :items="[
            {id:'420',label:'07:00'},{id:'480',label:'08:00'},{id:'540',label:'09:00'}]" />
          <p class="muted">当前免打扰 {{ fmtMinute(form.quiet_start) }}–{{ fmtMinute(form.quiet_end) }}，按固定 UTC 偏移 {{ form.utc_offset_minutes }} 分钟计算。跨夏令时地区需手动更新。</p>
        </NkListSection>
      </section>
    </div>
    <section class="card policy">
      <h3>隐私与行为边界</h3>
      <p>仅使用设备提供的天气、闹钟、日程、待办和用户明确保存的记忆。不会声称看见你、推断情绪、诊断健康，也不会自动写入长期记忆。主动消息进入设备通知中心并简要显示到猫眼；微信和飞书默认不发送。</p>
      <p v-if="companion.state?.last_error" role="alert">最近一次生成失败（{{ companion.state.last_error }}），一分钟后再试，不计作已送达消息。</p>
    </section>
  </div>
</template>

<style scoped>
.feature,.card { display:flex; flex-direction:column; gap:16px; }.grid { display:grid; gap:16px; }.desktop .grid,.tablet .grid { grid-template-columns:1fr 1.2fr; }
.card { padding:16px; border-radius:var(--radius-l); background:var(--md-surface-container); }.hero { align-items:center; text-align:center; }.hero.on { background:var(--md-primary-container); }
.hero-icon { width:88px; height:88px; display:grid; place-items:center; border-radius:50%; background:var(--md-surface-container-highest); }.hero h2,.hero p,.policy p,h3 { margin:0; }.muted { color:var(--md-on-surface-variant); }
</style>
