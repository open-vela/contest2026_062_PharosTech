<script setup lang="ts">
import { onMounted } from 'vue';
import { MdButton } from '@nyabula/ui';
import { useWeatherStore, weatherKind } from '../../../stores/weather';
import { useEyeStore } from '../../../stores/eye';
import type { FeatureMiniProps } from './contract';
defineProps<FeatureMiniProps>();
const weather = useWeatherStore();
const eye = useEyeStore();
onMounted(() => void weather.refresh());
function show(): void {
  if (!weather.now.temperature) return;
  void eye.setScene('weather', eye.sceneStyle, { city:weather.now.location?.name,
    temp:Math.round(weather.now.temperature.value), condition:weatherKind(weather.now.condition?.code) });
}
</script>
<template><div class="weather-mini">
  <span v-if="weather.now.temperature">{{ weather.now.location?.name }} · {{ Math.round(weather.now.temperature.value) }}° · {{ weather.now.condition?.text }}</span>
  <span v-else>尚无设备天气数据，请打开天气配置</span>
  <MdButton variant="tonal" :disabled="!weather.now.temperature" @click="show">显示</MdButton>
</div></template>
<style scoped>.weather-mini { display:flex; gap:12px; align-items:center; justify-content:space-between; }</style>
