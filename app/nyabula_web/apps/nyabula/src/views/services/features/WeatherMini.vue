<script setup lang="ts">
import { onMounted } from 'vue';
import { useWeatherStore } from '../../../stores/weather';
import { useWeatherScene } from './sceneLinks';
import type { FeatureMiniProps } from './contract';
import EyeShowButton from './EyeShowButton.vue';
const props = defineProps<FeatureMiniProps>();
const weather = useWeatherStore();
const scene = useWeatherScene(props.type);
onMounted(() => void weather.refresh());
</script>
<template><div class="weather-mini">
  <span v-if="weather.now.temperature">{{ weather.now.location?.name }} · {{ Math.round(weather.now.temperature.value) }}° · {{ weather.now.condition?.text }}</span>
  <span v-else>尚无设备天气数据，请打开天气配置</span>
  <EyeShowButton :shown="active || scene.held.value" :disabled="!scene.canShow.value" @toggle="scene.toggle()" />
</div></template>
<style scoped>.weather-mini { display:flex; gap:12px; align-items:center; justify-content:space-between; }</style>
