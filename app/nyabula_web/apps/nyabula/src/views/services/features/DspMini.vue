<script setup lang="ts">
import { computed } from 'vue';
import { useFeatureStatus } from '../../../composables/useFeatureStatus';
import { expandLegacyBands, eqResponse, validBands, type EqBand } from '../../../lib/dspEqualizer';
const props=defineProps<{type:string}>();
interface Dsp { bassSuppressed?:boolean; bassPresent:boolean; running:boolean; config:{eqEnabled:boolean;bypass:boolean;masterDb:number;bassEnabled:boolean;crossoverHz:number;satelliteHighpassHz?:number;bassLowpassHz?:number;delayMs:number[];eqBands:EqBand[];eqBands16?:EqBand[];eqDb:number[]} }
const { session,status,readError }=useFeatureStatus<Dsp>('dsp.status');
const config=computed(()=>status.value?.config);
const bands=computed(()=>config.value ? expandLegacyBands(config.value.eqBands16??config.value.eqBands,config.value.eqDb):[]);
const curve=computed(()=>validBands(bands.value)?Array.from({length:81},(_,i)=>`${i?'L':'M'}${i*3},${Math.max(2,Math.min(46,24-eqResponse(bands.value,20*1000**(i/80))*1.6))}`).join(' '):'');
const enabled=computed(()=>!!config.value && !config.value.bypass && (props.type==='eq'?config.value.eqEnabled:config.value.bassEnabled&&!status.value?.bassSuppressed));
</script>
<template>
  <div class="dsp-mini" v-if="config">
    <div class="summary"><strong>{{ type==='eq' ? (enabled?'16 段音色调节':'原声直通') : (status?.bassSuppressed ? '耳机模式 · 2.1 已停用' : enabled ? (status?.bassPresent?'2.1 双路输出':'等待 USB 声卡') : '低音输出关闭') }}</strong><span class="state" :class="{on:enabled}">{{ enabled?'已启用':'未启用' }}</span></div>
    <template v-if="type==='eq'"><svg viewBox="0 0 240 48" role="img" aria-label="当前均衡器频响曲线" preserveAspectRatio="none"><path d="M0,24H240" class="axis"/><path v-if="curve" :d="enabled?curve:'M0,24H240'" class="curve"/></svg><div class="foot"><span>20 Hz — 20 kHz</span><span>总增益 {{ config.masterDb }} dB</span></div></template>
    <div v-else class="metrics"><div><strong>{{ config.bassLowpassHz ?? config.crossoverHz }}<small> Hz</small></strong><span>低音低通</span></div><div><strong>{{ config.satelliteHighpassHz ?? config.crossoverHz }}<small> Hz</small></strong><span>主箱高通</span></div><div><strong>{{ config.delayMs?.[2] ?? 0 }}<small> ms</small></strong><span>低音延迟</span></div></div>
    <p v-if="readError" role="alert">{{ readError }}</p>
  </div>
  <p v-else class="empty">{{ readError || (session.connected?'正在读取音频设置…':'设备离线') }}</p>
</template>
<style scoped>
.dsp-mini { display:grid; gap:8px; }
.summary,.foot { display:flex; justify-content:space-between; align-items:center; gap:8px; }
.summary strong { font-size:13px; font-weight:500; }
.state { font-size:11px; color:var(--md-on-surface-variant); }
.state.on { color:var(--md-primary); }
svg { width:100%; height:38px; overflow:visible; }
.axis { stroke:var(--md-outline-variant); stroke-dasharray:3 4; }
.curve { stroke:var(--md-primary); stroke-width:2; fill:none; vector-effect:non-scaling-stroke; }
.foot,.metrics span,.metrics small,.empty { font-size:11px; color:var(--md-on-surface-variant); }
.metrics { display:grid; grid-template-columns:repeat(3,1fr); gap:10px; padding-top:4px; }
.metrics>div { display:grid; gap:5px; }
.metrics strong { font-size:23px; font-variant-numeric:tabular-nums; }
.metrics small { font-weight:400; }
p { margin:0; font-size:12px; color:var(--md-error); }
</style>
