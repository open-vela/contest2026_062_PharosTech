<script setup lang="ts">
/* Recursive NyaUI node renderer — one branch per v1 component type.
 * Unknown types render a gray placeholder block (never crash the page). */
import { computed, inject } from 'vue';
import { MdButton, MdCard, MdChip } from '@nyabula/ui';
import { NYAUI_CTX } from '../context.js';
import { getNyaComponent } from '../registry.js';
import type { NyaUiNode } from '../types.js';
import NyaIcon from './NyaIcon.vue';
import NyaChart from './NyaChart.vue';
import NyaEye from './NyaEye.vue';

const props = defineProps<{ node: NyaUiNode }>();
const ctx = inject(NYAUI_CTX)!;

/* Registry lookup first: a registered component wins over the built-in branch. */
const registered = computed(() => getNyaComponent(props.node.type)?.component);

const p = computed(() => props.node.props ?? {});
const kids = computed(() => props.node.children ?? []);

function num(v: unknown, dflt: number): number {
  return typeof v === 'number' && Number.isFinite(v) ? v : dflt;
}
function str(v: unknown, dflt = ''): string {
  return typeof v === 'string' ? v : dflt;
}

/* ---- layout styles ---- */
const columnStyle = computed(() => ({
  display: 'flex',
  flexDirection: 'column' as const,
  gap: num(p.value.gap, 8) + 'px',
  padding: num(p.value.padding, 0) + 'px',
  alignItems: ({ start: 'flex-start', center: 'center', end: 'flex-end' } as Record<string, string>)[
    str(p.value.align)
  ] ?? 'stretch',
}));
const rowStyle = computed(() => ({
  display: 'flex',
  flexDirection: 'row' as const,
  gap: num(p.value.gap, 8) + 'px',
  flexWrap: (p.value.wrap === true ? 'wrap' : 'nowrap') as 'wrap' | 'nowrap',
  alignItems: ({ start: 'flex-start', stretch: 'stretch', end: 'flex-end' } as Record<string, string>)[
    str(p.value.align)
  ] ?? 'center',
}));
const gridStyle = computed(() => ({
  display: 'grid',
  gridTemplateColumns: `repeat(${Math.max(1, num(p.value.columns, 2))}, minmax(0, 1fr))`,
  gap: num(p.value.gap, 8) + 'px',
}));

/* ---- input plumbing ---- */
const value = computed(() => ctx.getValue(props.node.id) ?? p.value.value);

const chipOptions = computed(() =>
  (Array.isArray(p.value.options) ? p.value.options : []).map(String),
);
const multi = computed(() => p.value.multi === true);
function chipSelected(opt: string): boolean {
  const v = value.value;
  return multi.value ? Array.isArray(v) && v.includes(opt) : v === opt;
}
function toggleChip(opt: string): void {
  if (multi.value) {
    const cur = Array.isArray(value.value) ? (value.value as unknown[]).map(String) : [];
    const next = cur.includes(opt) ? cur.filter((c) => c !== opt) : [...cur, opt];
    ctx.setValue(props.node, next, true);
  } else {
    ctx.setValue(props.node, opt, true);
  }
}

function onSliderInput(ev: Event): void {
  // Optimistic local update while dragging — no event sent.
  ctx.setValue(props.node, Number((ev.target as HTMLInputElement).value), false);
}
function onSliderChange(ev: Event): void {
  // Release: send immediately (contract: slider fires on release, not drag).
  ctx.setValue(props.node, Number((ev.target as HTMLInputElement).value), true);
}
function onTextInput(ev: Event): void {
  ctx.setValue(props.node, (ev.target as HTMLInputElement).value, false);
}
function onColorInput(ev: Event): void {
  ctx.setValue(props.node, (ev.target as HTMLInputElement).value, false);
}
function onSwitchToggle(): void {
  ctx.setValue(props.node, !(value.value === true), true);
}

const trendGlyph = computed(
  () => (({ up: '↑', down: '↓', flat: '→' }) as Record<string, string>)[str(p.value.trend)] ?? '',
);
const tappable = computed(() => !!props.node.on?.tap);
</script>

<template>
  <!-- layout -->
  <component :is="registered" v-if="registered" :node="node" />
  <div v-else-if="node.type === 'column'" :style="columnStyle">
    <NyaUiNode v-for="(c, i) in kids" :key="c.id ?? i" :node="c" />
  </div>
  <div v-else-if="node.type === 'row'" :style="rowStyle">
    <NyaUiNode v-for="(c, i) in kids" :key="c.id ?? i" :node="c" />
  </div>
  <div v-else-if="node.type === 'grid'" :style="gridStyle">
    <NyaUiNode v-for="(c, i) in kids" :key="c.id ?? i" :node="c" />
  </div>
  <MdCard
    v-else-if="node.type === 'card'"
    :title="str(p.title) || undefined"
    :style="p.padding !== undefined ? { padding: num(p.padding, 16) + 'px' } : undefined"
  >
    <div class="nya-card-body">
      <NyaUiNode v-for="(c, i) in kids" :key="c.id ?? i" :node="c" />
    </div>
  </MdCard>
  <section v-else-if="node.type === 'section'" class="nya-section">
    <h4 class="nya-section-title">{{ str(p.title) }}</h4>
    <div class="nya-card-body">
      <NyaUiNode v-for="(c, i) in kids" :key="c.id ?? i" :node="c" />
    </div>
  </section>
  <div v-else-if="node.type === 'spacer'" :style="{ height: num(p.size, 8) + 'px', flex: 'none' }" />

  <!-- display -->
  <p
    v-else-if="node.type === 'text'"
    class="nya-text"
    :class="['v-' + (str(p.variant) || 'body'), str(p.color) ? 'c-' + str(p.color) : '']"
  >
    {{ str(p.text) }}
  </p>
  <NyaIcon v-else-if="node.type === 'icon'" :name="str(p.name)" :size="num(p.size, 24)" />
  <img
    v-else-if="node.type === 'image'"
    class="nya-image"
    :src="str(p.src)"
    :style="p.height !== undefined ? { height: num(p.height, 120) + 'px' } : undefined"
    alt=""
  />
  <div v-else-if="node.type === 'statCard'" class="nya-stat">
    <div class="nya-stat-head">
      <NyaIcon v-if="p.icon" :name="str(p.icon)" :size="18" />
      <span class="nya-stat-label">{{ str(p.label) }}</span>
    </div>
    <div class="nya-stat-value">
      {{ str(p.value, String(p.value ?? '')) }}<span v-if="p.unit" class="nya-stat-unit">{{ str(p.unit) }}</span>
      <span v-if="trendGlyph" class="nya-stat-trend" :class="'t-' + str(p.trend)">{{ trendGlyph }}</span>
    </div>
  </div>
  <div v-else-if="node.type === 'progress'" class="nya-progress">
    <div v-if="p.label" class="nya-progress-label">{{ str(p.label) }}</div>
    <div class="nya-progress-track" :class="{ indeterminate: p.indeterminate === true }">
      <div
        class="nya-progress-fill"
        :style="p.indeterminate === true ? undefined : { width: Math.min(1, Math.max(0, num(p.value, 0))) * 100 + '%' }"
      />
    </div>
  </div>
  <NyaChart
    v-else-if="node.type === 'chart'"
    :kind="str(p.kind) === 'bar' ? 'bar' : 'line'"
    :points="Array.isArray(p.points) ? (p.points as number[]) : []"
    :labels="Array.isArray(p.labels) ? (p.labels as string[]) : undefined"
    :height="num(p.height, 120)"
  />
  <NyaEye v-else-if="node.type === 'eye'" :mode="str(p.mode, 'idle')" :height="num(p.height, 120)" />
  <span v-else-if="node.type === 'badge'" class="nya-badge" :class="'tone-' + (str(p.tone) || 'info')">
    {{ str(p.text) }}
  </span>
  <div
    v-else-if="node.type === 'listTile'"
    class="nya-tile"
    :class="{ tappable }"
    :role="tappable ? 'button' : undefined"
    @click="tappable && ctx.fire(node, 'tap')"
  >
    <NyaIcon v-if="p.icon" :name="str(p.icon)" :size="22" />
    <div class="nya-tile-main">
      <div class="nya-tile-title">{{ str(p.title) }}</div>
      <div v-if="p.subtitle" class="nya-tile-sub">{{ str(p.subtitle) }}</div>
    </div>
    <span v-if="p.trailing" class="nya-tile-trailing">{{ str(p.trailing) }}</span>
  </div>
  <div v-else-if="node.type === 'banner'" class="nya-banner" :class="'tone-' + (str(p.tone) || 'info')">
    {{ str(p.text) }}
  </div>

  <!-- input -->
  <MdButton
    v-else-if="node.type === 'button'"
    :variant="(str(p.variant) || 'filled') as 'filled' | 'tonal' | 'outlined' | 'text'"
    :disabled="p.disabled === true"
    class="nya-button"
    @click="ctx.fire(node, 'tap')"
  >
    <span class="nya-button-inner">
      <NyaIcon v-if="p.icon" :name="str(p.icon)" :size="18" />
      {{ str(p.text) }}
    </span>
  </MdButton>
  <div v-else-if="node.type === 'slider'" class="nya-field">
    <div v-if="p.label" class="nya-field-label">
      {{ str(p.label) }} <span class="nya-field-val">{{ num(value as number, num(p.min, 0)) }}</span>
    </div>
    <input
      type="range"
      class="nya-slider"
      :min="num(p.min, 0)"
      :max="num(p.max, 100)"
      :step="num(p.step, 1)"
      :value="num(value as number, num(p.min, 0))"
      @input="onSliderInput"
      @change="onSliderChange"
    />
  </div>
  <label v-else-if="node.type === 'switch'" class="nya-switch">
    <span class="nya-switch-label">{{ str(p.label) }}</span>
    <button
      type="button"
      class="nya-switch-track"
      :class="{ on: value === true }"
      role="switch"
      :aria-checked="value === true"
      @click="onSwitchToggle"
    >
      <span class="nya-switch-thumb" />
    </button>
  </label>
  <div v-else-if="node.type === 'chips'" class="nya-field">
    <div v-if="p.label" class="nya-field-label">{{ str(p.label) }}</div>
    <div class="nya-chips">
      <MdChip v-for="opt in chipOptions" :key="opt" :selected="chipSelected(opt)" @click="toggleChip(opt)">
        {{ opt }}
      </MdChip>
    </div>
  </div>
  <div v-else-if="node.type === 'textField'" class="nya-field">
    <div v-if="p.label" class="nya-field-label">{{ str(p.label) }}</div>
    <input
      type="text"
      class="nya-input"
      :value="str(value as string)"
      :placeholder="str(p.placeholder)"
      :maxlength="num(p.maxLength, 256)"
      @input="onTextInput"
    />
  </div>
  <div v-else-if="node.type === 'colorPicker'" class="nya-field">
    <label class="nya-color-row">
      <input type="color" class="nya-color" :value="str(value as string, '#62DFAF')" @input="onColorInput" />
      <span v-if="p.label" class="nya-field-label">{{ str(p.label) }}</span>
      <code class="nya-color-hex">{{ str(value as string, '#62DFAF') }}</code>
    </label>
  </div>

  <!-- unknown type -->
  <div v-else class="nya-unknown">{{ node.type }}</div>
</template>

<style scoped>
.nya-card-body {
  display: flex;
  flex-direction: column;
  gap: 10px;
}
.nya-section-title {
  font-size: 13px;
  letter-spacing: 1px;
  color: var(--md-on-surface-variant);
  margin-bottom: 8px;
}
.nya-text {
  color: var(--md-on-surface);
  margin: 0;
}
.nya-text.v-body { font: 400 14px var(--font-body); }
.nya-text.v-title { font: 600 17px var(--font-body); }
.nya-text.v-headline { font: 600 22px var(--font-title); }
.nya-text.v-label { font: 600 12px var(--font-body); color: var(--md-on-surface-variant); }
.nya-text.v-caption { font: 400 12px var(--font-body); color: var(--md-on-surface-variant); }
.nya-text.c-primary { color: var(--md-primary); }
.nya-text.c-muted { color: var(--md-on-surface-variant); }
.nya-text.c-error { color: var(--md-error); }
.nya-image {
  max-width: 100%;
  border-radius: var(--radius-m);
  object-fit: cover;
}
.nya-stat {
  flex: 1;
  min-width: 0;
  background: var(--md-surface-container-high);
  border-radius: var(--radius-m);
  padding: 12px 14px;
}
.nya-stat-head {
  display: flex;
  align-items: center;
  gap: 6px;
  color: var(--md-on-surface-variant);
  font: 600 12px var(--font-body);
}
.nya-stat-value {
  margin-top: 6px;
  font: 700 24px var(--font-title);
  color: var(--md-on-surface);
}
.nya-stat-unit {
  font: 600 13px var(--font-body);
  color: var(--md-on-surface-variant);
  margin-left: 3px;
}
.nya-stat-trend { font-size: 14px; margin-left: 6px; }
.nya-stat-trend.t-up { color: var(--md-success); }
.nya-stat-trend.t-down { color: var(--md-warning); }
.nya-stat-trend.t-flat { color: var(--md-on-surface-variant); }
.nya-progress-label {
  font: 600 12px var(--font-body);
  color: var(--md-on-surface-variant);
  margin-bottom: 6px;
}
.nya-progress-track {
  height: 6px;
  border-radius: 3px;
  background: var(--md-surface-container-highest);
  overflow: hidden;
  position: relative;
}
.nya-progress-fill {
  height: 100%;
  border-radius: 3px;
  background: var(--md-primary);
  transition: width 0.3s;
}
.nya-progress-track.indeterminate .nya-progress-fill {
  position: absolute;
  width: 35%;
  animation: nya-indet 1.4s ease-in-out infinite;
}
@keyframes nya-indet {
  0% { left: -35%; }
  100% { left: 100%; }
}
.nya-badge {
  display: inline-block;
  padding: 3px 10px;
  border-radius: var(--radius-full);
  font: 600 12px var(--font-body);
  width: fit-content;
}
.nya-badge.tone-ok, .nya-banner.tone-ok { background: var(--md-primary-container); color: var(--md-on-primary-container); }
.nya-badge.tone-warn, .nya-banner.tone-warn { background: rgba(255, 217, 77, 0.18); color: var(--md-warning); }
.nya-badge.tone-error, .nya-banner.tone-error { background: rgba(255, 180, 171, 0.15); color: var(--md-error); }
.nya-badge.tone-info, .nya-banner.tone-info { background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.nya-banner {
  padding: 10px 14px;
  border-radius: var(--radius-m);
  font: 500 13px var(--font-body);
}
.nya-tile {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 10px 12px;
  border-radius: var(--radius-m);
  color: var(--md-on-surface);
}
.nya-tile.tappable { cursor: pointer; }
.nya-tile.tappable:hover { background: var(--md-surface-container-high); }
.nya-tile-main { flex: 1; min-width: 0; }
.nya-tile-title { font: 600 14px var(--font-body); }
.nya-tile-sub { font: 400 12px var(--font-body); color: var(--md-on-surface-variant); margin-top: 2px; }
.nya-tile-trailing { font: 600 13px var(--font-body); color: var(--md-on-surface-variant); }
.nya-button-inner {
  display: inline-flex;
  align-items: center;
  gap: 6px;
}
.nya-field-label {
  font: 600 12px var(--font-body);
  color: var(--md-on-surface-variant);
  margin-bottom: 6px;
}
.nya-field-val { color: var(--md-primary); margin-left: 4px; }
.nya-slider {
  -webkit-appearance: none;
  appearance: none;
  width: 100%;
  height: 4px;
  border-radius: 2px;
  background: var(--md-surface-container-highest);
  outline: none;
  accent-color: var(--md-primary);
}
.nya-slider::-webkit-slider-thumb {
  -webkit-appearance: none;
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: var(--md-primary);
  cursor: pointer;
  border: none;
}
.nya-slider::-moz-range-thumb {
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: var(--md-primary);
  cursor: pointer;
  border: none;
}
.nya-switch {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}
.nya-switch-label {
  font: 500 14px var(--font-body);
  color: var(--md-on-surface);
}
.nya-switch-track {
  width: 46px;
  height: 26px;
  border-radius: 13px;
  border: 1px solid var(--md-outline);
  background: var(--md-surface-container-highest);
  position: relative;
  cursor: pointer;
  transition: background 0.15s, border-color 0.15s;
  flex: none;
  padding: 0;
}
.nya-switch-track.on {
  background: var(--md-primary);
  border-color: var(--md-primary);
}
.nya-switch-thumb {
  position: absolute;
  top: 3px;
  left: 3px;
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: var(--md-on-surface-variant);
  transition: left 0.15s, background 0.15s;
}
.nya-switch-track.on .nya-switch-thumb {
  left: 23px;
  background: var(--md-on-primary);
}
.nya-chips {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}
.nya-input {
  width: 100%;
  background: var(--md-surface-container-high);
  border: 1px solid var(--md-outline-variant);
  border-radius: var(--radius-s);
  color: var(--md-on-surface);
  font: 400 14px var(--font-body);
  padding: 9px 12px;
  outline: none;
}
.nya-input:focus { border-color: var(--md-primary); }
.nya-color-row {
  display: flex;
  align-items: center;
  gap: 10px;
  cursor: pointer;
}
.nya-color-row .nya-field-label { margin-bottom: 0; }
.nya-color {
  width: 34px;
  height: 26px;
  border: none;
  padding: 0;
  background: transparent;
  cursor: pointer;
}
.nya-color-hex {
  font: 600 12px monospace;
  color: var(--md-on-surface-variant);
}
.nya-unknown {
  background: var(--md-surface-container-highest);
  color: var(--md-on-surface-variant);
  border: 1px dashed var(--md-outline-variant);
  border-radius: var(--radius-s);
  padding: 10px 12px;
  font: 600 12px monospace;
}
</style>
