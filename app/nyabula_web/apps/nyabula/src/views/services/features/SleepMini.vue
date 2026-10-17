<script setup lang="ts">
/* Sleep mini: the sleep switch. It drives the device eye expression
 * ('sleep', or a release back to the agent's own expression). */
import { computed } from 'vue';
import { MdSwitch, UiIcon } from '@nyabula/ui';
import { MODE_LABELS, useEyeStore } from '../../../stores/eye';
import { useSessionStore } from '../../../stores/session';
import type { FeatureMiniProps } from './contract';

defineProps<FeatureMiniProps>();
const eye = useEyeStore();
const session = useSessionStore();

const asleep = computed(() => eye.activeMode === 'sleep');
const note = computed(() => (!session.connected ? '设备离线' : asleep.value ? '眼睛已闭上' : `当前表情：${MODE_LABELS[eye.activeMode] ?? eye.activeMode}`));
function setAsleep(v: boolean): void {
  void eye.setMode(v ? 'sleep' : 'idle');
}
</script>

<template>
  <div class="slm">
    <label class="slm-sw">
      <UiIcon name="moon" :size="18" :class="{ on: asleep }" />
      <span>{{ asleep ? '休眠中' : '休眠' }}</span>
      <MdSwitch :model-value="asleep" :disabled="!session.canControl" @update:model-value="setAsleep" />
    </label>
    <span class="slm-note">{{ note }}</span>
  </div>
</template>

<style scoped>
.slm-note { flex: 1; min-width: 0; font-size: 12.5px; color: var(--md-on-surface-variant); text-align: right; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.slm { display: flex; align-items: center; gap: 12px; min-height: 44px; }
.slm-sw { display: flex; align-items: center; gap: 6px; min-height: 40px; flex: none; font: 600 13px var(--font-body); color: var(--md-on-surface); cursor: pointer; }
.slm-sw .on { color: var(--md-primary); }
.slm-bright { flex: 1; min-width: 0; display: flex; align-items: center; gap: 8px; }
.ic { color: var(--md-on-surface-variant); flex: none; }
.slm-bright :deep(.md-slider) { flex: 1; min-width: 50px; }
.pct { font-size: 12px; color: var(--md-on-surface-variant); min-width: 34px; text-align: right; font-variant-numeric: tabular-nums; }
</style>
