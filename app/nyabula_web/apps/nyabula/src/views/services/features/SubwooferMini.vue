<script setup lang="ts">
/* Subwoofer mini: enable switch + strength slider (pushed on release).
 * Shares nyabula.feature.subwoofer with SubwooferFeature; pushes
 * {level, enabled} on every action. */
import { ref, watch } from 'vue';
import { MdSlider, MdSwitch, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor; active: boolean; payload: Record<string, unknown> | null }>();
const eye = useEyeStore();

const mem = useFeatureMemory(props.type, { level: 40, enabled: true });
const level = ref(mem.level);
const enabled = ref(mem.enabled);
watch(() => [props.active, props.payload] as const, ([a, pl]) => {
  if (!a || !pl) return;
  if (typeof pl.level === 'number') level.value = pl.level;
  if (typeof pl.enabled === 'boolean') enabled.value = pl.enabled;
}, { immediate: true });

function push(): void {
  saveFeatureMemory(props.type, { level: level.value, enabled: enabled.value });
  void eye.setScene(props.type, eye.sceneStyle, { level: level.value, enabled: enabled.value });
}
function setEnabled(v: boolean): void {
  enabled.value = v;
  push();
}
</script>

<template>
  <div class="sw">
    <span class="sw-toggle"><MdSwitch :model-value="enabled" @update:model-value="setEnabled" /></span>
    <UiIcon name="volume_down" :size="18" class="ic" />
    <MdSlider v-model="level" :min="0" :max="100" :disabled="!enabled" @change="push" />
    <span class="pct mono">{{ enabled ? level + '%' : '关' }}</span>
  </div>
</template>

<style scoped>
.sw { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.sw-toggle { display: inline-flex; align-items: center; min-height: 40px; flex: none; }
.ic { color: var(--md-on-surface-variant); flex: none; }
.sw :deep(.md-slider) { flex: 1; min-width: 60px; }
.pct { font-size: 12px; color: var(--md-on-surface-variant); min-width: 32px; text-align: right; font-variant-numeric: tabular-nums; }
</style>
