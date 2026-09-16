<script setup lang="ts">
/* Hour:minute wheel picker (alarm clock). modelValue is "HH:MM". Each column
 * is a native scroll-snap list so touch scrolling and mouse wheel work out of
 * the box; arrow keys step the focused column. Value commits when scrolling
 * settles. */
import { computed, nextTick, onMounted, ref, watch } from 'vue';

export interface NkTimeWheelProps {
  modelValue: string;
  /** Minute granularity (1, 5, 10, 15, 30). */
  minuteStep?: number;
  disabled?: boolean;
}
const props = withDefaults(defineProps<NkTimeWheelProps>(), { minuteStep: 1, disabled: false });
const emit = defineEmits<{ (e: 'update:modelValue', v: string): void }>();

const ITEM_H = 44;
const hours = Array.from({ length: 24 }, (_, i) => i);
const minutes = computed(() => {
  const step = Math.max(1, Math.min(30, Math.floor(props.minuteStep)));
  return Array.from({ length: Math.ceil(60 / step) }, (_, i) => i * step);
});
const pad = (n: number): string => String(n).padStart(2, '0');

function parse(v: string): { h: number; m: number } {
  const m = /^(\d{1,2}):(\d{1,2})$/.exec(v ?? '');
  const h = m ? Math.min(23, Math.max(0, Number(m[1]))) : 0;
  const mm = m ? Math.min(59, Math.max(0, Number(m[2]))) : 0;
  return { h, m: mm };
}
const cur = computed(() => parse(props.modelValue));

const hourEl = ref<HTMLElement>();
const minEl = ref<HTMLElement>();
let settleTimer: ReturnType<typeof setTimeout> | undefined;
let syncing = false;

function scrollToIndex(el: HTMLElement | undefined, idx: number, smooth = false): void {
  if (!el) return;
  syncing = true;
  el.scrollTo({ top: idx * ITEM_H, behavior: smooth ? 'smooth' : 'auto' });
  setTimeout(() => (syncing = false), smooth ? 350 : 30);
}
function syncFromModel(smooth = false): void {
  const { h, m } = cur.value;
  scrollToIndex(hourEl.value, h, smooth);
  const mi = minutes.value.findIndex((x) => x >= m);
  scrollToIndex(minEl.value, mi < 0 ? minutes.value.length - 1 : mi, smooth);
}
function commit(): void {
  if (props.disabled || syncing) return;
  const hi = Math.round((hourEl.value?.scrollTop ?? 0) / ITEM_H);
  const mi = Math.round((minEl.value?.scrollTop ?? 0) / ITEM_H);
  const h = hours[Math.min(hours.length - 1, Math.max(0, hi))];
  const m = minutes.value[Math.min(minutes.value.length - 1, Math.max(0, mi))];
  const next = `${pad(h)}:${pad(m)}`;
  if (next !== props.modelValue) emit('update:modelValue', next);
}
function onScroll(): void {
  if (settleTimer) clearTimeout(settleTimer);
  settleTimer = setTimeout(commit, 120);
}
function onKey(ev: KeyboardEvent, col: 'h' | 'm'): void {
  if (props.disabled) return;
  const dir = ev.key === 'ArrowUp' ? -1 : ev.key === 'ArrowDown' ? 1 : 0;
  if (!dir) return;
  ev.preventDefault();
  const { h, m } = cur.value;
  if (col === 'h') {
    emit('update:modelValue', `${pad((h + dir + 24) % 24)}:${pad(m)}`);
  } else {
    const list = minutes.value;
    const i = Math.max(0, list.findIndex((x) => x >= m));
    emit('update:modelValue', `${pad(h)}:${pad(list[(i + dir + list.length) % list.length])}`);
  }
}
function pick(col: 'h' | 'm', v: number): void {
  if (props.disabled) return;
  const { h, m } = cur.value;
  emit('update:modelValue', col === 'h' ? `${pad(v)}:${pad(m)}` : `${pad(h)}:${pad(v)}`);
}

onMounted(() => nextTick(() => syncFromModel(false)));
watch(() => props.modelValue, () => nextTick(() => syncFromModel(true)));
</script>

<template>
  <div class="nk-wheel" :class="{ disabled }" role="group" aria-label="时间">
    <div class="nk-wheel-highlight" aria-hidden="true" />
    <div ref="hourEl" class="nk-wheel-col" tabindex="0" role="listbox" aria-label="小时" @scroll.passive="onScroll" @keydown="onKey($event, 'h')">
      <div class="nk-wheel-pad" />
      <div v-for="h in hours" :key="h" class="nk-wheel-item" :class="{ on: h === cur.h }" role="option" :aria-selected="h === cur.h" @click="pick('h', h)">{{ pad(h) }}</div>
      <div class="nk-wheel-pad" />
    </div>
    <span class="nk-wheel-colon">:</span>
    <div ref="minEl" class="nk-wheel-col" tabindex="0" role="listbox" aria-label="分钟" @scroll.passive="onScroll" @keydown="onKey($event, 'm')">
      <div class="nk-wheel-pad" />
      <div v-for="m in minutes" :key="m" class="nk-wheel-item" :class="{ on: m === cur.m }" role="option" :aria-selected="m === cur.m" @click="pick('m', m)">{{ pad(m) }}</div>
      <div class="nk-wheel-pad" />
    </div>
  </div>
</template>

<style scoped>
.nk-wheel {
  --nk-item: 44px;
  position: relative;
  display: flex; align-items: center; justify-content: center; gap: 6px;
  height: calc(var(--nk-item) * 5);
  color: var(--md-on-surface);
  user-select: none;
  -webkit-mask-image: linear-gradient(transparent, #000 25%, #000 75%, transparent);
  mask-image: linear-gradient(transparent, #000 25%, #000 75%, transparent);
}
.nk-wheel.disabled { opacity: 0.45; pointer-events: none; }
.nk-wheel-highlight {
  position: absolute; left: 0; right: 0; top: calc(var(--nk-item) * 2); height: var(--nk-item);
  border-radius: var(--radius-m);
  background: var(--md-surface-container-high);
  pointer-events: none;
}
.nk-wheel-col {
  position: relative;
  height: 100%; width: 88px;
  overflow-y: auto; overscroll-behavior: contain;
  scroll-snap-type: y mandatory;
  scrollbar-width: none;
  outline: none;
}
.nk-wheel-col::-webkit-scrollbar { display: none; }
.nk-wheel-col:focus-visible { box-shadow: inset 0 0 0 2px var(--md-primary); border-radius: var(--radius-m); }
.nk-wheel-pad { height: calc(var(--nk-item) * 2); flex: none; }
.nk-wheel-item {
  height: var(--nk-item); display: grid; place-items: center;
  scroll-snap-align: center;
  font: 600 26px var(--font-title); font-variant-numeric: tabular-nums;
  color: var(--md-on-surface-variant);
  cursor: pointer;
  transition: color var(--dur-fast), transform var(--dur-fast);
}
.nk-wheel-item.on { color: var(--md-primary); transform: scale(1.12); }
.nk-wheel-colon { font: 700 28px var(--font-title); color: var(--md-on-surface-variant); }
</style>
