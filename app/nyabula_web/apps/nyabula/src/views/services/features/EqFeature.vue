<script setup lang="ts">
/* Equalizer feature: preset chips plus five hand-drawn vertical band sliders
 * (-12..+12 dB). Dragging a band switches the preset to "custom". Mirrors
 * {preset, bands} to the device while shown. */
import { computed, ref, watch } from 'vue';
import { NkActionBar, NkChipSelect, NkListSection } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();

const BANDS = ['60', '250', '1k', '4k', '12k'];
const PRESETS: Record<string, number[]> = {
  flat: [0, 0, 0, 0, 0],
  pop: [-1, 2, 4, 2, -1],
  rock: [5, 3, -1, 2, 4],
  classical: [4, 2, -1, 2, 3],
  vocal: [-2, 1, 4, 3, 0],
};
const PRESET_OPTIONS = [
  { id: 'flat', label: '标准' },
  { id: 'pop', label: '流行' },
  { id: 'rock', label: '摇滚' },
  { id: 'classical', label: '古典' },
  { id: 'vocal', label: '人声' },
  { id: 'custom', label: '自定义' },
];
const presetLabel = (id: string): string => PRESET_OPTIONS.find((o) => o.id === id)?.label ?? id;
const MIN = -12;
const MAX = 12;

const mem = useFeatureMemory(props.type, { preset: 'flat', bands: PRESETS.flat! });
const preset = ref<string | string[]>(mem.preset);
const bands = ref<number[]>(BANDS.map((_, i) => Number(mem.bands[i] ?? 0)));
const presetId = computed(() => (Array.isArray(preset.value) ? preset.value[0] ?? 'flat' : preset.value));
const isShown = computed(() => eye.activeScene === props.type);

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { preset: presetId.value, bands: [...bands.value] });
}
function persist(): void {
  saveFeatureMemory(props.type, { preset: presetId.value, bands: [...bands.value] });
}
watch(preset, () => {
  const p = PRESETS[presetId.value];
  if (p) bands.value = [...p];
  persist();
  if (isShown.value) push();
});
function setBand(i: number, v: number): void {
  const clamped = Math.round(Math.min(MAX, Math.max(MIN, v)));
  if (bands.value[i] === clamped) return;
  bands.value = bands.value.map((b, j) => (j === i ? clamped : b));
  if (presetId.value !== 'custom') preset.value = 'custom';
  persist();
}
function commitBand(): void {
  if (isShown.value) push();
}
/* Pointer drag on the vertical track. */
function startDrag(i: number, ev: PointerEvent): void {
  const el = ev.currentTarget as HTMLElement;
  el.setPointerCapture(ev.pointerId);
  const rect = el.getBoundingClientRect();
  const apply = (e: PointerEvent) => {
    const ratio = 1 - Math.min(1, Math.max(0, (e.clientY - rect.top) / rect.height));
    setBand(i, MIN + ratio * (MAX - MIN));
  };
  apply(ev);
  const move = (e: PointerEvent) => apply(e);
  const up = () => {
    el.removeEventListener('pointermove', move);
    el.removeEventListener('pointerup', up);
    el.removeEventListener('pointercancel', up);
    commitBand();
  };
  el.addEventListener('pointermove', move);
  el.addEventListener('pointerup', up);
  el.addEventListener('pointercancel', up);
}
function onKey(i: number, ev: KeyboardEvent): void {
  const cur = bands.value[i] ?? 0;
  if (ev.key === 'ArrowUp') setBand(i, cur + 1);
  else if (ev.key === 'ArrowDown') setBand(i, cur - 1);
  else if (ev.key === 'Home') setBand(i, 0);
  else return;
  ev.preventDefault();
  commitBand();
}
const pct = (v: number): number => ((v - MIN) / (MAX - MIN)) * 100;
const fmtDb = (v: number): string => (v > 0 ? `+${v}` : String(v));
function toggleShown(): void {
  if (isShown.value) void eye.setScene(null);
  else push();
}
</script>

<template>
  <div class="feature" :class="ff">
    <div class="grid">
      <section class="col">
        <NkListSection title="预设">
          <div class="chips"><NkChipSelect v-model="preset" :options="PRESET_OPTIONS" /></div>
        </NkListSection>
        <NkActionBar
          :primary-text="isShown ? '退出显示' : '在设备上显示'"
          :primary-icon="isShown ? 'close' : 'visibility'"
          secondary-text="重置"
          secondary-icon="refresh"
          @primary="toggleShown"
          @secondary="preset = 'flat'"
        />
        <p class="muted sub">当前：{{ presetLabel(presetId) }}</p>
      </section>
      <section class="col">
        <NkListSection title="频段">
          <div class="bands">
            <div v-for="(name, i) in BANDS" :key="name" class="band">
              <span class="db mono">{{ fmtDb(bands[i] ?? 0) }}</span>
              <div
                class="track"
                role="slider"
                tabindex="0"
                :aria-label="name + ' Hz'"
                :aria-valuemin="MIN"
                :aria-valuemax="MAX"
                :aria-valuenow="bands[i] ?? 0"
                @pointerdown.prevent="startDrag(i, $event)"
                @keydown="onKey(i, $event)"
              >
                <span class="zero" />
                <span class="fill" :style="{ height: pct(bands[i] ?? 0) + '%' }" />
                <span class="knob" :style="{ bottom: pct(bands[i] ?? 0) + '%' }" />
              </div>
              <span class="name">{{ name }}</span>
            </div>
          </div>
        </NkListSection>
      </section>
    </div>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.grid { display: grid; grid-template-columns: 1fr; gap: 16px; align-items: start; }
.feature.desktop .grid { grid-template-columns: 1fr 1.2fr; }
.col { display: flex; flex-direction: column; gap: 14px; min-width: 0; }
.chips { padding: 8px; }
.sub { margin: 0; font-size: 12.5px; text-align: center; }
.bands { display: grid; grid-template-columns: repeat(5, minmax(44px, 1fr)); gap: 8px; padding: 12px 8px 8px; }
.band { display: flex; flex-direction: column; align-items: center; gap: 8px; }
.db { font-size: 12px; color: var(--md-primary); font-weight: 600; min-height: 16px; }
.name { font-size: 12px; color: var(--md-on-surface-variant); }
.track {
  position: relative;
  width: 44px;
  height: 200px;
  border-radius: 22px;
  background: var(--md-surface-container-highest);
  cursor: pointer;
  touch-action: none;
  outline: none;
}
.track:focus-visible { box-shadow: 0 0 0 2px var(--md-primary); }
.zero { position: absolute; left: 10px; right: 10px; top: 50%; height: 2px; background: var(--md-outline-variant); border-radius: 1px; }
.fill { position: absolute; left: 0; right: 0; bottom: 0; border-radius: 22px; background: var(--md-primary-container); transition: height var(--dur-fast) var(--ease-out); }
.knob {
  position: absolute;
  left: 4px;
  width: 36px;
  height: 36px;
  margin-bottom: -18px;
  border-radius: 50%;
  background: var(--md-primary);
  box-shadow: 0 2px 6px rgba(0, 0, 0, 0.2);
  transition: bottom var(--dur-fast) var(--ease-out);
}
.feature.phone .track { height: 180px; }
</style>
