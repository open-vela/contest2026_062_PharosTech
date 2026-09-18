<script setup lang="ts">
/* Subwoofer feature: gauge showing the current bass level, a slider for
 * low-frequency boost and an enable switch. Mirrors {level, enabled} to the
 * device while shown. */
import { computed, ref, watch } from 'vue';
import { NkActionBar, NkGauge, NkListSection, NkSliderRow, NkToggleRow } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();

const mem = useFeatureMemory(props.type, { level: 40, enabled: true });
const level = ref(mem.level);
const enabled = ref(mem.enabled);
const isShown = computed(() => eye.activeScene === props.type);
const gaugeValue = computed(() => (enabled.value ? level.value : 0));
const strengthText = computed(() => {
  if (!enabled.value) return '已关闭';
  if (level.value >= 80) return '震撼';
  if (level.value >= 50) return '饱满';
  if (level.value >= 20) return '轻柔';
  return '几乎无';
});

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { level: level.value, enabled: enabled.value });
}
watch([level, enabled], () => saveFeatureMemory(props.type, { level: level.value, enabled: enabled.value }));
watch(enabled, () => {
  if (isShown.value) push();
});
function onCommit(): void {
  if (isShown.value) push();
}
function toggleShown(): void {
  if (isShown.value) void eye.setScene(null);
  else push();
}
</script>

<template>
  <div class="feature" :class="ff">
    <div class="grid">
      <section class="card gauge-card">
        <NkGauge :value="gaugeValue" label="低频强度" :size="ff === 'phone' ? 180 : 220" :stroke="16" :warn-at="85" />
        <span class="strength">{{ strengthText }}</span>
      </section>
      <section class="col">
        <NkListSection title="调节">
          <NkToggleRow v-model="enabled" icon="subwoofer" title="低音炮" sub="关闭后不增强低频" />
          <NkSliderRow v-model="level" title="低频增强" unit="%" icon-start="volume_down" icon-end="volume_up" :disabled="!enabled" @commit="onCommit" />
        </NkListSection>
        <NkActionBar
          :primary-text="isShown ? '退出显示' : '在设备上显示'"
          :primary-icon="isShown ? 'close' : 'visibility'"
          @primary="toggleShown"
        />
      </section>
    </div>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.grid { display: grid; grid-template-columns: 1fr; gap: 16px; align-items: start; }
.feature.desktop .grid { grid-template-columns: 1fr 1fr; }
.col { display: flex; flex-direction: column; gap: 14px; min-width: 0; }
.card {
  display: flex; flex-direction: column; gap: 12px; align-items: center;
  padding: 20px; border-radius: var(--radius-l);
  background: var(--md-surface-container); color: var(--md-on-surface);
}
.strength { font: 600 15px var(--font-body); color: var(--md-on-surface-variant); }
</style>
