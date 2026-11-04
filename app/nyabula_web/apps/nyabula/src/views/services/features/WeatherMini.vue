<script setup lang="ts">
import { computed, watch } from 'vue';
import { UiIcon } from '@nyabula/ui';
import { useWeatherStore, weatherKind } from '../../../stores/weather';
import { useVisiblePoll } from '../../../composables/usePageVisible';
import { useWeatherScene } from './sceneLinks';
import type { FeatureMiniProps } from './contract';
import EyeShowButton from './EyeShowButton.vue';
const props=defineProps<FeatureMiniProps>();
const emit=defineEmits<{(e:'open'):void}>();
const weather=useWeatherStore();const scene=useWeatherScene(props.type);
const place=computed(()=>weather.now.location?.name||weather.status?.city||'选择城市');
const day=computed(()=>weather.daily.days?.[0]);
const emptyText=computed(()=>!weather.available?'设备未连接':!weather.status?.city?'接入当地真实天气':weather.status.refreshing?'正在获取天气':weather.error||weather.status.last_error?'更新失败，可重试':'等待天气更新');
watch(()=>weather.available,ready=>{if(ready)void weather.refresh();},{immediate:true});
useVisiblePoll(()=>void weather.refresh(),10000);
</script>
<template>
 <div class="weather-mini">
  <div class="weather-main"><UiIcon :name="weatherKind(weather.now.condition?.code)==='sunny'?'light_mode':'cloud'" :size="32"/><div class="weather-place"><strong>{{place}}</strong><span>{{weather.now.condition?.text||emptyText}}</span></div><strong v-if="weather.now.temperature" class="temperature">{{Math.round(weather.now.temperature.value)}}<small>°</small></strong></div>
  <div class="weather-foot"><span v-if="day">{{Math.round(day.temperatureMin.value)}}° / {{Math.round(day.temperatureMax.value)}}°</span><button v-else-if="weather.status?.city" :disabled="weather.status.refreshing" @click="weather.fetchNow()">{{weather.status.refreshing?'更新中…':'刷新天气'}}</button><button v-else @click="emit('open')">设置城市<UiIcon name="chevron_right" :size="15"/></button><EyeShowButton v-if="weather.now.temperature" :shown="active||scene.held.value" :disabled="!scene.canShow.value" @toggle="scene.toggle()"/><button v-else-if="weather.status?.city" @click="emit('open')">设置</button></div>
 </div>
</template>
<style scoped>
.weather-mini { display:grid;gap:12px; }.weather-main { display:flex;align-items:center;gap:12px; }.weather-main>.ui-icon { color:var(--md-primary);flex:none; }.weather-place { display:grid;gap:4px;flex:1;min-width:0; }.weather-place strong { font-size:14px; }.weather-place span { color:var(--md-on-surface-variant);font-size:12px; }.temperature { font-size:36px;font-weight:500;line-height:1;font-variant-numeric:tabular-nums; }.temperature small { font-size:24px; }.weather-foot { display:flex;justify-content:space-between;align-items:center;gap:12px;min-height:28px; }.weather-foot>span { color:var(--md-on-surface-variant);font-size:12px; }button { display:inline-flex;align-items:center;gap:3px;padding:0;border:0;background:transparent;color:var(--md-primary);font:600 12px var(--font-body);cursor:pointer; }button:disabled { opacity:.4;cursor:default; }
</style>
