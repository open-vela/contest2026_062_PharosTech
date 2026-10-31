<script setup lang="ts">
/* Core-owned timer view, shared with the full feature page. */
import { computed } from 'vue';
import { UiIcon } from '@nyabula/ui';
import { useCoreTimer } from '../../../composables/useCoreTimer';
import type { FormFactor } from '../../../composables/useFormFactor';
defineProps<{ type: string; ff: FormFactor; active: boolean; payload: Record<string, unknown> | null }>();
const timer = useCoreTimer('stopwatch');
const running = timer.running;
const armed = computed(() => timer.current.value !== null);
const disabled = computed(() => timer.core.busy.value || !timer.core.available.value);
const elapsed = timer.elapsed;
const display = computed(() => {
  const ms = elapsed.value;
  const s = Math.floor(ms / 1000);
  return String(Math.floor(s / 60)).padStart(2, '0') + ':' + String(s % 60).padStart(2, '0') + '.' + String(Math.floor(ms % 1000 / 10)).padStart(2, '0');
});
async function start() {
  if (armed.value) await timer.action('resume'); else await timer.create(0, '秒表');
}
async function stop() { await timer.action('pause'); }
async function reset() { await timer.action('delete'); }
</script>

<template>
  <div class="sm">
    <span class="big mono" :class="{ live: running }">{{ display }}</span>
    <button type="button" class="rb main" :disabled="disabled || timer.finished.value || timer.waitingClock.value" :class="{ stop: running }" :aria-label="running ? '停止' : '开始'" @click="running ? stop() : start()">
      <UiIcon :name="running ? 'stop' : 'play_arrow'" :size="22" />
    </button>
    <button type="button" class="rb" aria-label="重置" :disabled="disabled || running || !armed" @click="reset"><UiIcon name="refresh" :size="20" /></button>
  </div>
  <p v-if="timer.core.error.value" role="alert">{{ timer.core.error.value }}</p>
</template>

<style scoped>
.sm { display: flex; align-items: center; gap: 8px; min-height: 44px; }
.big { flex: 1; min-width: 0; font-size: 28px; font-weight: 700; line-height: 1; font-variant-numeric: tabular-nums; color: var(--md-on-surface); white-space: nowrap; }
.big.live { color: var(--md-primary); }
.rb { width: 40px; height: 40px; border-radius: 50%; border: none; cursor: pointer; display: grid; place-items: center; background: transparent; color: var(--md-on-surface); flex: none; }
.rb:hover:not(:disabled) { background: var(--md-surface-container-highest); }
.rb:disabled { opacity: 0.35; cursor: default; }
.rb.main { width: 44px; height: 44px; background: var(--md-primary); color: var(--md-on-primary); }
.rb.main.stop { background: var(--md-error); color: var(--md-on-error); }
</style>
