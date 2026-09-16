<script setup lang="ts">
/* Equalizer mini: one horizontally scrolling row of preset chips; tapping
 * a preset saves it (nyabula.feature.eq, shared with EqFeature) and pushes
 * {preset, bands} to the device. The live preset highlights while active. */
import { computed } from 'vue';
import { MdChip } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor; active: boolean; payload: Record<string, unknown> | null }>();
const eye = useEyeStore();

const PRESETS: Record<string, number[]> = {
  flat: [0, 0, 0, 0, 0],
  pop: [-1, 2, 4, 2, -1],
  rock: [5, 3, -1, 2, 4],
  classical: [4, 2, -1, 2, 3],
  vocal: [-2, 1, 4, 3, 0],
};
const OPTIONS = [
  { id: 'flat', label: '标准' },
  { id: 'pop', label: '流行' },
  { id: 'rock', label: '摇滚' },
  { id: 'classical', label: '古典' },
  { id: 'vocal', label: '人声' },
];
const mem = useFeatureMemory(props.type, { preset: 'flat', bands: PRESETS.flat! });
const current = computed(() => (props.active && typeof props.payload?.preset === 'string' ? (props.payload.preset as string) : mem.preset));

function pick(id: string): void {
  const bands = [...(PRESETS[id] ?? PRESETS.flat!)];
  mem.preset = id;
  mem.bands = bands;
  saveFeatureMemory(props.type, { preset: id, bands });
  void eye.setScene(props.type, eye.sceneStyle, { preset: id, bands });
}
</script>

<template>
  <div class="eq">
    <MdChip v-for="o in OPTIONS" :key="o.id" class="chip" :selected="current === o.id" @click="pick(o.id)">{{ o.label }}</MdChip>
    <MdChip v-if="current === 'custom'" class="chip" selected>自定义</MdChip>
  </div>
</template>

<style scoped>
.eq { display: flex; gap: 8px; overflow-x: auto; padding: 2px 0; scrollbar-width: none; -webkit-overflow-scrolling: touch; }
.eq::-webkit-scrollbar { display: none; }
.chip { flex: none; min-height: 40px; }
</style>
