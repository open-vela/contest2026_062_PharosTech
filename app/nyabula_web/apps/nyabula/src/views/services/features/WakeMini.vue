<script setup lang="ts">
import { computed } from 'vue';
import { useFeatureStatus } from '../../../composables/useFeatureStatus';
interface Voice {enabled:boolean;streaming:boolean;microphone:{open:boolean;level:number;claims:number;blockedByPlayback?:boolean};config:{wakeSensitivity:number;wakePhrase?:string};test?:{active:boolean}}
const {session,status,readError}=useFeatureStatus<Voice>('voice.status');
const label=computed(()=>!session.connected?'设备离线':!status.value?'正在读取…':status.value.test?.active?'正在测试':!status.value.enabled?'已关闭':status.value.microphone.claims||status.value.microphone.blockedByPlayback?'播放占用麦克风':status.value.streaming?'等待唤醒':'准备中');
const level=computed(()=>status.value?.microphone.open?Math.min(100,Math.max(0,(20*Math.log10(Math.max(1,status.value.microphone.level)/32768)+60)/.6)):0);
</script>
<template><div class="wake-mini"><div><strong>{{ status?.config.wakePhrase || '你好，openvela' }}</strong><span>{{ label }}</span></div><div class="level" aria-label="设备麦克风电平"><i :style="{width:`${level}%`}"/></div><small>{{ readError || `灵敏度 ${status?.config.wakeSensitivity ?? '—'} · 进入可测试唤醒与麦克风` }}</small></div></template>
<style scoped>
.wake-mini { display:grid; gap:10px; }.wake-mini>div:first-child { display:flex; justify-content:space-between; gap:10px; }.wake-mini strong { font-size:14px; }.wake-mini span,.wake-mini small { font-size:12px; color:var(--md-on-surface-variant); }.level { height:6px; border-radius:5px; overflow:hidden; background:var(--md-surface-container-highest); }.level i { display:block; height:100%; border-radius:inherit; background:var(--md-primary); transition:width .2s ease; }
</style>
