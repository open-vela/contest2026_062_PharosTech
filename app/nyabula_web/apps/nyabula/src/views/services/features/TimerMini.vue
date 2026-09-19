<script setup lang="ts">
/* Core-owned timer view, shared with the full feature page. */
import { computed } from 'vue';
import { MdChip, UiIcon } from '@nyabula/ui';
import { useCoreTimer } from '../../../composables/useCoreTimer';
import type { FormFactor } from '../../../composables/useFormFactor';
import EyeShowButton from './EyeShowButton.vue';
defineProps<{ type: string; ff: FormFactor; active: boolean; payload: Record<string, unknown> | null }>();
const timer = useCoreTimer('countdown');
const running = timer.running;
const scene = timer.scene;
const armed = computed(() => timer.current.value !== null);
const disabled = computed(() => timer.core.busy.value || !timer.core.available.value);
const PRESETS = [1, 3, 5, 10, 25];
const display = computed(() => {
  const ms = timer.remaining.value;
  const s = Math.ceil(ms / 1000);
  return String(Math.floor(s / 60)).padStart(2, '0') + ':' + String(s % 60).padStart(2, '0');
});
const progress = computed(() => timer.current.value?.duration_ms ? timer.remaining.value / timer.current.value.duration_ms * 100 : 0);
async function start(minutes = 0) {
  await timer.create(minutes * 60000, '倒计时');
}
async function pause() { await timer.action('pause'); }
async function resume() { await timer.action('resume'); }
async function reset() { await timer.action('delete'); }
</script>

<template>
  <div v-if="!armed" class="tm chips">
    <MdChip v-for="m in PRESETS" :key="m" class="chip" :disabled="disabled" @click="start(m)">{{ m }} 分</MdChip>
  </div>
  <div v-else class="tm run">
    <div class="clock">
      <span class="big mono" :class="{ live: running }">{{ display }}</span>
      <span class="bar"><span class="fill" :style="{ width: progress + '%' }" /></span>
    </div>
    <button type="button" class="rb main" :disabled="disabled || timer.finished.value || timer.waitingClock.value" :aria-label="running ? '暂停' : '继续'" @click="running ? pause() : resume()"><UiIcon :name="running ? 'pause' : 'play_arrow'" :size="22" /></button>
    <button type="button" class="rb" aria-label="重置" :disabled="disabled" @click="reset"><UiIcon name="refresh" :size="20" /></button>
    <EyeShowButton kind="round" :shown="active || scene.held.value" :disabled="!scene.canShow.value" @toggle="scene.toggle()" />
  </div>
  <p v-if="timer.core.error.value" role="alert">{{ timer.core.error.value }}</p>
</template>

<style scoped>
.chips { display: flex; gap: 8px; overflow-x: auto; scrollbar-width: none; padding: 2px 0; }
.chips::-webkit-scrollbar { display: none; }
.chip { flex: none; min-height: 40px; }
.run { display: flex; align-items: center; gap: 8px; }
.clock { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 6px; }
.big { font-size: 30px; font-weight: 700; line-height: 1; font-variant-numeric: tabular-nums; color: var(--md-on-surface); }
.big.live { color: var(--md-primary); }
.bar { display: block; height: 4px; border-radius: 2px; background: var(--md-surface-container-highest); overflow: hidden; }
.fill { display: block; height: 100%; background: var(--md-primary); transition: width 0.4s linear; }
.rb { width: 40px; height: 40px; border-radius: 50%; border: none; cursor: pointer; display: grid; place-items: center; background: transparent; color: var(--md-on-surface); flex: none; }
.rb:hover { background: var(--md-surface-container-highest); }
.rb.main { width: 44px; height: 44px; background: var(--md-primary); color: var(--md-on-primary); }
</style>
