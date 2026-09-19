<script setup lang="ts">
/* Core-owned timer view, shared with the full feature page. */
import { computed } from 'vue';
import { MdChip, UiIcon } from '@nyabula/ui';
import { useCoreTimer } from '../../../composables/useCoreTimer';
import type { FormFactor } from '../../../composables/useFormFactor';
import EyeShowButton from './EyeShowButton.vue';
defineProps<{ type: string; ff: FormFactor; active: boolean; payload: Record<string, unknown> | null }>();
const timer = useCoreTimer('sleep');
const scene = timer.scene;
const armed = computed(() => timer.current.value !== null);
const disabled = computed(() => timer.core.busy.value || !timer.core.available.value);
const PRESETS = [15, 30, 60];
const display = computed(() => {
  const ms = timer.remaining.value;
  const s = Math.ceil(ms / 1000);
  return String(Math.floor(s / 60)).padStart(2, '0') + ':' + String(s % 60).padStart(2, '0');
});
const actionText = computed(() => timer.waitingClock.value ? '等待校时' : timer.finished.value ? '计时完成，猫眼已休眠' : timer.running.value ? '设备计时中' : '已暂停');
async function start(minutes = 0) {
  await timer.create(minutes * 60000, '睡眠定时');
}
async function cancel() { await timer.action('delete'); }
</script>

<template>
  <div v-if="!armed" class="st chips">
    <MdChip v-for="m in PRESETS" :key="m" class="chip" :disabled="disabled" @click="start(m)">{{ m }} 分钟</MdChip>
  </div>
  <div v-else class="st run">
    <div class="clock">
      <span class="big mono">{{ display }}</span>
      <span class="sub">{{ actionText }}</span>
    </div>
    <EyeShowButton kind="round" :shown="active || scene.held.value" :disabled="!scene.canShow.value" @toggle="scene.toggle()" />
    <button type="button" class="cancel" :disabled="disabled" aria-label="取消" @click="cancel"><UiIcon name="close" :size="18" /><span>取消</span></button>
  </div>
  <p v-if="timer.core.error.value" role="alert">{{ timer.core.error.value }}</p>
</template>

<style scoped>
.chips { display: flex; gap: 8px; overflow-x: auto; scrollbar-width: none; padding: 2px 0; }
.chips::-webkit-scrollbar { display: none; }
.chip { flex: none; min-height: 40px; }
.run { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.clock { flex: 1; min-width: 0; display: flex; align-items: baseline; gap: 8px; }
.big { font-size: 28px; font-weight: 700; line-height: 1; font-variant-numeric: tabular-nums; color: var(--md-primary); }
.sub { font-size: 12.5px; color: var(--md-on-surface-variant); }
.cancel {
  display: inline-flex; align-items: center; gap: 2px; min-height: 40px; padding: 0 14px 0 10px; flex: none;
  border: 1px solid var(--md-outline-variant); border-radius: var(--radius-full); cursor: pointer;
  background: transparent; color: var(--md-on-surface); font: 600 13px var(--font-body);
}
.cancel:hover { background: var(--md-surface-container-highest); }
</style>
