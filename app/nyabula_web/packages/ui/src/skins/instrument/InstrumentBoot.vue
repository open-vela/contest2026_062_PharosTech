<script setup lang="ts">
/* Instrument boot overlay: a power-on self test. A dot-matrix wordmark,
 * a monospace log where every finished boot stage (loading.bootLabel)
 * is stamped [ OK ], a segmented LED progress bar driven by bootProgress,
 * then a CRT power-off exit (collapse to a scan line, then to a point)
 * that reveals the app underneath. Contract: after bootDone the overlay
 * exits and clears loading.bootOverlayVisible (see ClassicBoot.vue). */
import { computed, ref, watch } from 'vue';
import { useLoadingStore } from '../../stores/loading';
import DotMatrix from './DotMatrix.vue';

const loading = useLoadingStore();
const SEGMENTS = 32;
const EXIT_MS = 720;

const exiting = ref(false);
const removed = ref(false);
const done = ref<string[]>([]);
const t0 = performance.now();

function stamp(): string {
  return ((performance.now() - t0) / 1000).toFixed(3).padStart(7, ' ');
}
interface LogLine { t: string; text: string }
const log = ref<LogLine[]>([{ t: stamp(), text: 'NYABULA OS · POST' }]);

watch(
  () => loading.bootLabel,
  (label, prev) => {
    if (prev && !done.value.includes(prev)) {
      done.value.push(prev);
      log.value.push({ t: stamp(), text: prev });
    }
  },
);

const lit = computed(() => Math.round((Math.min(100, Math.max(0, loading.bootProgress)) / 100) * SEGMENTS));
const pct = computed(() => String(Math.round(loading.bootProgress)).padStart(3, '0') + '%');

watch(
  () => loading.bootDone,
  (isDone) => {
    if (!isDone) return;
    if (!done.value.includes(loading.bootLabel)) log.value.push({ t: stamp(), text: loading.bootLabel });
    window.setTimeout(() => {
      exiting.value = true;
      window.setTimeout(() => {
        removed.value = true;
        loading.bootOverlayVisible = false;
      }, EXIT_MS);
    }, 260);
  },
  { immediate: true },
);
</script>

<template>
  <div v-if="!removed" class="ins-boot" :class="{ exiting }" aria-live="polite">
    <div class="frame">
      <header class="top">
        <span>NYABULA / K7</span>
        <span>RK3576 · OPENVELA</span>
      </header>

      <div class="mark">
        <DotMatrix text="NYABULA" :dot="7" :gap="2.5" class="word" />
      </div>

      <ol class="log">
        <li v-for="(l, i) in log" :key="i" class="line">
          <span class="ts">[{{ l.t }}]</span>
          <span class="msg">{{ l.text }}</span>
          <span class="ok">OK</span>
        </li>
        <li v-if="!loading.bootDone" class="line live">
          <span class="ts">[{{ stamp() }}]</span>
          <span class="msg">{{ loading.bootLabel }}<i class="cursor" /></span>
        </li>
      </ol>

      <div class="meter">
        <div class="bar" role="progressbar" :aria-valuenow="loading.bootProgress" aria-valuemin="0" aria-valuemax="100">
          <i v-for="n in SEGMENTS" :key="n" :class="{ on: n <= lit }" />
        </div>
        <DotMatrix :text="pct" :dot="3" :gap="1.2" class="pct" />
      </div>
    </div>
  </div>
</template>

<style scoped>
.ins-boot {
  --mono: ui-monospace, 'Cascadia Mono', 'JetBrains Mono', Consolas, monospace;
  position: fixed;
  inset: 0;
  z-index: 9999;
  display: grid;
  place-items: center;
  background-color: var(--md-surface);
  background-image:
    linear-gradient(var(--ins-grid, rgba(127, 127, 127, 0.06)) 1px, transparent 1px),
    linear-gradient(90deg, var(--ins-grid, rgba(127, 127, 127, 0.06)) 1px, transparent 1px);
  background-size: 24px 24px;
  color: var(--md-on-surface);
  font: 500 12px/1.6 var(--mono);
  overflow: hidden;
  transform-origin: 50% 50%;
}
.frame {
  width: min(520px, calc(100vw - 40px));
  display: flex;
  flex-direction: column;
  gap: 22px;
}
.top {
  display: flex;
  justify-content: space-between;
  font-size: 10.5px;
  letter-spacing: 0.16em;
  color: var(--md-on-surface-variant);
  padding-bottom: 10px;
  border-bottom: 1px solid var(--md-outline-variant);
  animation: ins-in 0.2s steps(3, jump-none) both;
}
.mark { display: flex; justify-content: flex-start; color: var(--md-on-surface); }
.word { max-width: 100%; height: auto; animation: ins-in 0.32s steps(4, jump-none) 0.06s both; }
.log {
  list-style: none;
  margin: 0;
  padding: 0;
  min-height: 7.4em;
  display: flex;
  flex-direction: column;
  justify-content: flex-end;
  overflow: hidden;
}
.line { display: flex; gap: 10px; white-space: nowrap; animation: ins-in 0.16s steps(2, jump-none) both; }
.ts { color: var(--md-outline); }
.msg { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; }
.ok { color: var(--md-primary); letter-spacing: 0.1em; }
.live .msg { color: var(--md-on-surface); }
.cursor {
  display: inline-block;
  width: 0.6em;
  height: 1.05em;
  margin-left: 4px;
  vertical-align: -0.18em;
  background: var(--md-primary);
  animation: ins-cursor 0.9s steps(2, jump-none) infinite;
}
.meter { display: flex; align-items: center; gap: 14px; }
.bar { flex: 1; display: flex; gap: 2px; height: 12px; }
.bar i { flex: 1; background: var(--md-outline-variant); transition: background 0.12s steps(2, jump-none), box-shadow 0.12s; }
.bar i.on { background: var(--md-primary); box-shadow: 0 0 6px rgba(var(--md-primary-rgb), 0.6); }
.pct { color: var(--md-primary); }

/* CRT power-off: squash to a scan line, then pinch the line to a dot. */
.ins-boot.exiting { animation: crt-off 0.7s cubic-bezier(0.7, 0, 0.84, 0) forwards; background-image: none; }
.ins-boot.exiting .frame { animation: frame-out 0.3s steps(3, jump-none) forwards; }
@keyframes frame-out { to { opacity: 0; } }
@keyframes crt-off {
  0% { transform: scale(1, 1); opacity: 1; }
  40% { background-color: var(--md-surface); }
  55% { transform: scale(1, 0.004); opacity: 1; background-color: var(--md-on-surface); }
  88% { transform: scale(0, 0.004); opacity: 1; background-color: var(--md-primary); }
  100% { transform: scale(0, 0.004); opacity: 0; }
}
@keyframes ins-in { from { opacity: 0; } to { opacity: 1; } }
@keyframes ins-cursor { 50% { opacity: 0; } }
@media (prefers-reduced-motion: reduce) {
  .ins-boot.exiting { animation: fade 0.2s linear forwards; }
  @keyframes fade { to { opacity: 0; } }
}
</style>
