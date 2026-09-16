<script setup lang="ts">
/* Weather card with inline SVG glyphs for a small condition vocabulary. */
import { computed } from 'vue';

export type NkWeatherKind = 'sunny' | 'cloudy' | 'rain' | 'snow' | 'storm' | 'fog';
export interface NkWeatherCardProps {
  city: string;
  temp: number | string;
  unit?: string;
  text?: string;
  kind?: NkWeatherKind;
  high?: number | string;
  low?: number | string;
  /** Extra caption, e.g. humidity or AQI. */
  extra?: string;
}
const props = withDefaults(defineProps<NkWeatherCardProps>(), { unit: '°', kind: 'sunny' });

const KIND_LABEL: Record<NkWeatherKind, string> = {
  sunny: '晴',
  cloudy: '多云',
  rain: '雨',
  snow: '雪',
  storm: '雷暴',
  fog: '雾',
};
const label = computed(() => props.text || KIND_LABEL[props.kind] || '');
</script>

<template>
  <div class="nk-weather" :class="'k-' + kind">
    <div class="nk-weather-main">
      <div class="nk-weather-city">{{ city }}</div>
      <div class="nk-weather-temp">{{ temp }}<span class="nk-weather-unit">{{ unit }}</span></div>
      <div class="nk-weather-text">
        {{ label }}
        <span v-if="high !== undefined || low !== undefined" class="nk-weather-range">
          <template v-if="high !== undefined">{{ high }}{{ unit }}</template>
          <template v-if="high !== undefined && low !== undefined"> / </template>
          <template v-if="low !== undefined">{{ low }}{{ unit }}</template>
        </span>
      </div>
      <div v-if="extra" class="nk-weather-extra">{{ extra }}</div>
    </div>
    <svg class="nk-weather-glyph" viewBox="0 0 64 64" aria-hidden="true">
      <!-- sun -->
      <g v-if="kind === 'sunny'" class="sun">
        <circle cx="32" cy="32" r="12" />
        <g stroke-width="3" stroke-linecap="round">
          <line x1="32" y1="6" x2="32" y2="13" /><line x1="32" y1="51" x2="32" y2="58" />
          <line x1="6" y1="32" x2="13" y2="32" /><line x1="51" y1="32" x2="58" y2="32" />
          <line x1="13.6" y1="13.6" x2="18.5" y2="18.5" /><line x1="45.5" y1="45.5" x2="50.4" y2="50.4" />
          <line x1="13.6" y1="50.4" x2="18.5" y2="45.5" /><line x1="45.5" y1="18.5" x2="50.4" y2="13.6" />
        </g>
      </g>
      <!-- cloud base for cloudy / rain / snow / storm -->
      <g v-else-if="kind !== 'fog'">
        <path class="cloud" d="M20 44a9 9 0 0 1-1-18 13 13 0 0 1 25-3 10 10 0 0 1 4 19.6z" />
        <g v-if="kind === 'rain'" class="drops" stroke-width="3" stroke-linecap="round">
          <line x1="22" y1="48" x2="19" y2="56" /><line x1="32" y1="48" x2="29" y2="56" /><line x1="42" y1="48" x2="39" y2="56" />
        </g>
        <g v-else-if="kind === 'snow'" class="flakes">
          <circle cx="22" cy="52" r="2.5" /><circle cx="32" cy="55" r="2.5" /><circle cx="42" cy="52" r="2.5" />
        </g>
        <path v-else-if="kind === 'storm'" class="bolt" d="M34 44l-7 10h6l-3 8 9-12h-6l3-6z" />
      </g>
      <!-- fog -->
      <g v-else class="fog" stroke-width="4" stroke-linecap="round">
        <line x1="12" y1="24" x2="52" y2="24" /><line x1="18" y1="34" x2="46" y2="34" /><line x1="12" y1="44" x2="52" y2="44" />
      </g>
    </svg>
  </div>
</template>

<style scoped>
.nk-weather {
  display: flex; align-items: center; justify-content: space-between; gap: 12px;
  padding: 16px 18px;
  border-radius: var(--radius-l);
  background: linear-gradient(135deg, var(--md-primary-container), var(--md-surface-container-high));
  color: var(--md-on-surface);
}
.nk-weather-city { font: 600 14px var(--font-body); color: var(--md-on-surface-variant); }
.nk-weather-temp { margin-top: 4px; font: 700 40px var(--font-title); line-height: 1; }
.nk-weather-unit { font: 600 20px var(--font-body); vertical-align: top; margin-left: 2px; }
.nk-weather-text { margin-top: 6px; font: 600 14px var(--font-body); }
.nk-weather-range { margin-left: 8px; font-weight: 500; color: var(--md-on-surface-variant); }
.nk-weather-extra { margin-top: 2px; font: 400 12px var(--font-body); color: var(--md-on-surface-variant); }
.nk-weather-glyph { width: 88px; height: 88px; flex: none; }
.sun circle { fill: var(--md-warning); }
.sun line { stroke: var(--md-warning); }
.cloud { fill: var(--md-on-surface-variant); opacity: 0.85; }
.drops line { stroke: var(--md-tertiary); }
.flakes circle { fill: var(--md-on-surface); }
.bolt { fill: var(--md-warning); }
.fog line { stroke: var(--md-on-surface-variant); opacity: 0.8; }
</style>
