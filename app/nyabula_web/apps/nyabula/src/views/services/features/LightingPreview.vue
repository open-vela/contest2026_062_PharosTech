<script setup lang="ts">
import { computed, useId } from 'vue';
import type { LightZone } from '../../../lib/lighting';
import { lightingArtwork } from '../../../lib/lightingArtwork';
import front from '../../../assets/lighting/nyabula-front.png';
import light from '../../../assets/lighting/nyabula-light.png';
const props = defineProps<{ pixels?: number[]; selected?: LightZone; compact?: boolean; iris?: readonly string[] }>();
const id = `ny-light-${useId().replace(/[^a-zA-Z0-9_-]/g, '')}`;
const layers = computed(() => lightingArtwork(props.pixels, props.iris));
const regions = [
  { id: 'left_ear', x: 40, y: 25, width: 112, height: 125 },
  { id: 'right_ear', x: 208, y: 25, width: 112, height: 125 },
  { id: 'chest', x: 164, y: 318, width: 32, height: 82 },
  { id: 'base', x: 64, y: 424, width: 232, height: 42 },
];
const zones = computed(() => regions.map(region => ({ ...region, pixels: layers.value.filter(layer => layer.zone === region.id) })));
const eyes = computed(() => layers.value.filter(layer => !layer.zone));
</script>
<template>
  <svg class="lighting-preview" :class="{ compact }" viewBox="32 10 296 464" role="img" aria-label="Nyabula 左耳、右耳、胸口和底座灯光预览">
    <defs>
      <mask :id="`${id}-light-mask`" maskUnits="userSpaceOnUse" x="0" y="0" width="360" height="480" style="mask-type:luminance"><image :href="light" width="360" height="480" /></mask>
      <filter :id="`${id}-off`" color-interpolation-filters="sRGB"><feColorMatrix type="matrix" values=".12 0 0 0 0 0 .12 0 0 0 0 0 .12 0 0 0 0 0 1 0" /></filter>
      <filter :id="`${id}-bloom`"><feGaussianBlur stdDeviation="1.5" /></filter>
      <filter :id="`${id}-chest-footprint`"><feMorphology in="SourceAlpha" operator="dilate" radius="3" /><feGaussianBlur stdDeviation="1.2" /></filter>
      <filter :id="`${id}-chest-base`" color-interpolation-filters="sRGB"><feColorMatrix type="matrix" values=".5 0 0 0 0 0 .5 0 0 0 0 0 .5 0 0 0 0 0 1 0" /></filter>
      <mask :id="`${id}-chest-surround`" maskUnits="userSpaceOnUse" x="164" y="308" width="32" height="104" style="mask-type:alpha"><image :href="light" width="360" height="480" :filter="`url(#${id}-chest-footprint)`" /></mask>
      <template v-for="zone in zones" :key="zone.id">
        <clipPath :id="`${id}-${zone.id}-clip`"><rect :x="zone.x" :y="zone.y" :width="zone.width" :height="zone.height" /></clipPath>
        <linearGradient :id="`${id}-${zone.id}-gradient`" x1="0" y1="0" :x2="zone.id === 'chest' ? '0' : '1'" :y2="zone.id === 'chest' ? '1' : '0'" color-interpolation="sRGB">
          <stop v-for="(pixel, index) in zone.pixels" :key="pixel.id" :data-pixel="pixel.pixel" :offset="`${zone.pixels.length === 1 ? 0 : index / (zone.pixels.length - 1) * 100}%`" :stop-color="pixel.color" />
        </linearGradient>
      </template>
      <template v-for="eye in eyes" :key="eye.id">
        <clipPath :id="`${id}-${eye.id}-clip`"><rect :x="eye.x" :y="eye.y" :width="eye.width" :height="eye.height" /></clipPath>
        <filter :id="`${id}-${eye.id}-color`" color-interpolation-filters="sRGB"><feColorMatrix type="matrix" :values="eye.matrix" /></filter>
      </template>
    </defs>
    <image :href="front" width="360" height="480" class="robot-body" />
    <image :href="front" width="360" height="480" :mask="`url(#${id}-chest-surround)`" :filter="`url(#${id}-chest-base)`" />
    <g v-for="zone in zones" :key="zone.id" :data-zone="zone.id" :data-selected="zone.id === selected">
      <image :href="light" width="360" height="480" :clip-path="`url(#${id}-${zone.id}-clip)`" :filter="`url(#${id}-off)`" />
      <g :mask="`url(#${id}-light-mask)`">
        <rect :x="zone.x" :y="zone.y" :width="zone.width" :height="zone.height" :fill="`url(#${id}-${zone.id}-gradient)`" />
      </g>
      <g v-if="zone.pixels.some(pixel => pixel.glow !== 'none')" :filter="`url(#${id}-bloom)`" opacity=".35">
        <rect :x="zone.x" :y="zone.y" :width="zone.width" :height="zone.height" :fill="`url(#${id}-${zone.id}-gradient)`" :mask="`url(#${id}-light-mask)`" />
      </g>
    </g>
    <image v-for="eye in eyes" :key="eye.id" :href="light" width="360" height="480" :clip-path="`url(#${id}-${eye.id}-clip)`" :filter="`url(#${id}-${eye.id}-color)`" />
  </svg>
</template>
<style scoped>
.lighting-preview { display:block;width:220px;max-width:100%;height:auto;margin:auto;overflow:visible; }
.robot-body { filter:drop-shadow(0 8px 9px rgba(0,0,0,.25)); }
.lighting-preview.compact { width:100px;height:157px; }
</style>
