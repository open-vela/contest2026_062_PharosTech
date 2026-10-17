<script setup lang="ts">
/* Duration dial for countdown timers. modelValue is total seconds. Two big
 * segments (minutes / seconds); tap a segment to focus it, then +/- buttons,
 * vertical drag on the segment, mouse wheel or arrow keys adjust it. Preset
 * chips set the whole value. */
import { computed, ref } from 'vue';
import UiIcon from '../components/UiIcon.vue';

export interface NkDialPreset {
  label: string;
  seconds: number;
}
export interface NkDialProps {
  modelValue: number;
  min?: number;
  max?: number;
  /** Step of the seconds segment. */
  step?: number;
  presets?: NkDialPreset[];
  disabled?: boolean;
}
const props = withDefaults(defineProps<NkDialProps>(), {
  min: 0,
  max: 99 * 60 + 59,
  step: 1,
  presets: () => [],
  disabled: false,
});
const emit = defineEmits<{ (e: 'update:modelValue', v: number): void }>();

type Seg = 'min' | 'sec';
const focus = ref<Seg>('min');
const total = computed(() => Math.min(props.max, Math.max(props.min, Math.floor(props.modelValue || 0))));
const minutes = computed(() => Math.floor(total.value / 60));
const seconds = computed(() => total.value % 60);
const pad = (n: number): string => String(n).padStart(2, '0');

function set(v: number): void {
  if (props.disabled) return;
  emit('update:modelValue', Math.min(props.max, Math.max(props.min, Math.round(v))));
}
function bump(dir: 1 | -1, seg: Seg = focus.value): void {
  set(total.value + dir * (seg === 'min' ? 60 : props.step));
}

/* Vertical drag: every 18px changes the focused segment by one unit. */
let dragStartY = 0;
let dragStartVal = 0;
let dragSeg: Seg = 'min';
function onPointerDown(ev: PointerEvent, seg: Seg): void {
  if (props.disabled) return;
  focus.value = seg;
  dragSeg = seg;
  dragStartY = ev.clientY;
  dragStartVal = total.value;
  (ev.currentTarget as HTMLElement).setPointerCapture(ev.pointerId);
}
function onPointerMove(ev: PointerEvent): void {
  if (!(ev.buttons & 1)) return;
  const units = Math.round((dragStartY - ev.clientY) / 18);
  if (units === 0) return;
  set(dragStartVal + units * (dragSeg === 'min' ? 60 : props.step));
}
function onWheel(ev: WheelEvent, seg: Seg): void {
  ev.preventDefault();
  focus.value = seg;
  bump(ev.deltaY < 0 ? 1 : -1, seg);
}
function onKey(ev: KeyboardEvent, seg: Seg): void {
  if (ev.key === 'ArrowUp') { ev.preventDefault(); bump(1, seg); }
  else if (ev.key === 'ArrowDown') { ev.preventDefault(); bump(-1, seg); }
}
</script>

<template>
  <div class="nk-dial" :class="{ disabled }">
    <div class="nk-dial-face">
      <button type="button" class="nk-dial-btn" aria-label="减少" :disabled="disabled" @click="bump(-1)">
        <UiIcon name="remove" :size="22" />
      </button>
      <div class="nk-dial-digits">
        <div
          class="nk-dial-seg"
          :class="{ focus: focus === 'min' }"
          tabindex="0"
          role="spinbutton"
          aria-label="分钟"
          :aria-valuenow="minutes"
          @pointerdown="onPointerDown($event, 'min')"
          @pointermove="onPointerMove"
          @wheel="onWheel($event, 'min')"
          @keydown="onKey($event, 'min')"
        >
          <span class="nk-dial-num">{{ pad(minutes) }}</span>
          <span class="nk-dial-unit">分</span>
        </div>
        <span class="nk-dial-colon">:</span>
        <div
          class="nk-dial-seg"
          :class="{ focus: focus === 'sec' }"
          tabindex="0"
          role="spinbutton"
          aria-label="秒"
          :aria-valuenow="seconds"
          @pointerdown="onPointerDown($event, 'sec')"
          @pointermove="onPointerMove"
          @wheel="onWheel($event, 'sec')"
          @keydown="onKey($event, 'sec')"
        >
          <span class="nk-dial-num">{{ pad(seconds) }}</span>
          <span class="nk-dial-unit">秒</span>
        </div>
      </div>
      <button type="button" class="nk-dial-btn" aria-label="增加" :disabled="disabled" @click="bump(1)">
        <UiIcon name="add" :size="22" />
      </button>
    </div>
    <div v-if="presets.length" class="nk-dial-presets">
      <button
        v-for="p in presets"
        :key="p.label"
        type="button"
        class="nk-dial-preset"
        :class="{ selected: p.seconds === total }"
        :disabled="disabled"
        @click="set(p.seconds)"
      >
        {{ p.label }}
      </button>
    </div>
  </div>
</template>

<style scoped>
.nk-dial { display: flex; flex-direction: column; gap: 14px; align-items: center; color: var(--md-on-surface); user-select: none; }
.nk-dial.disabled { opacity: 0.45; pointer-events: none; }
.nk-dial-face { display: flex; align-items: center; gap: 12px; }
.nk-dial-btn {
  width: 48px; height: 48px; flex: none;
  border: none; border-radius: var(--radius-full);
  background: var(--md-surface-container-highest);
  color: var(--md-on-surface);
  display: grid; place-items: center; cursor: pointer;
  transition: background var(--dur-fast), transform var(--dur-fast);
}
.nk-dial-btn:active { transform: scale(0.94); }
.nk-dial-btn:focus-visible { outline: 2px solid var(--md-primary); outline-offset: 2px; }
.nk-dial-digits { display: flex; align-items: center; gap: 4px; }
.nk-dial-seg {
  display: flex; flex-direction: column; align-items: center;
  min-width: 84px; padding: 8px 10px;
  border-radius: var(--radius-m);
  cursor: ns-resize; touch-action: none;
  transition: background var(--dur-fast);
}
.nk-dial-seg.focus { background: var(--md-surface-container-high); }
.nk-dial-seg:focus-visible { outline: 2px solid var(--md-primary); }
.nk-dial-num { font: 700 52px var(--font-title); line-height: 1; font-variant-numeric: tabular-nums; }
.nk-dial-seg.focus .nk-dial-num { color: var(--md-primary); }
.nk-dial-unit { font: 600 12px var(--font-body); color: var(--md-on-surface-variant); margin-top: 4px; }
.nk-dial-colon { font: 700 40px var(--font-title); color: var(--md-on-surface-variant); padding-bottom: 18px; }
.nk-dial-presets { display: flex; flex-wrap: wrap; justify-content: center; gap: 8px; }
.nk-dial-preset {
  min-height: 40px; padding: 8px 14px;
  border: 1px solid var(--md-outline-variant); border-radius: var(--radius-full);
  background: transparent; color: var(--md-on-surface-variant);
  font: 600 13px var(--font-body); cursor: pointer;
  transition: background var(--dur-fast), color var(--dur-fast);
}
.nk-dial-preset.selected { background: var(--md-secondary-container); color: var(--md-on-secondary-container); border-color: transparent; }
</style>
