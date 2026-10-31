<script setup lang="ts">
/* Original Wake presentation: transparent lid mask, night/aurora, greeting
 * and flight. Only the milestone text and extra expressions are removed.
 * Wait for the actual initial route before deciding whether an anchor exists. */
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref } from 'vue';
import { EyeEngine } from '@nyabula/eye-engine';
import { useLoadingStore } from '../../stores/loading';
const loading = useLoadingStore();
const host = ref<HTMLElement | null>(null);
const canvas = ref<HTMLCanvasElement | null>(null);
const phase = ref<'dusk' | 'open' | 'awake' | 'fly' | 'land'>('dusk');
const gone = ref(false);
const noAnchor = ref(false);
const box = reactive({ w: 0, h: 0, x: 0, y: 0 });
const flight = ref('');
let engine: EyeEngine | null = null;
let frame = 0;
let mirrorFrame = 0;
let bareEyes = 1;
let introPose: ReturnType<EyeEngine['capturePresentation']> | undefined;
let liveMix = 0;
let disposed = false;
const timers: number[] = [];
const later = (ms: number, fn: () => void) => timers.push(window.setTimeout(() => { if (!disposed) fn(); }, ms));
const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
function tween(ms: number, fn: (p: number) => void) {
  const start = performance.now();
  const step = (now: number) => {
    if (disposed) return;
    const p = Math.min(1, (now - start) / ms);
    fn(p * p * (3 - 2 * p));
    if (p < 1) frame = requestAnimationFrame(step);
  };
  frame = requestAnimationFrame(step);
}
/* Scattered stars from a seeded PRNG (stable across renders). */
function prng(seed: number) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const rnd = prng(20260924);
const STARS = Array.from({ length: 70 }, () => ({ x: rnd() * 100, y: rnd() * 100, s: rnd() < 0.12 ? 2 : 1, d: rnd() * 4.5, o: 0.15 + rnd() * 0.3 }));


const eyeBottom = computed(() => box.y + box.h * .46 + Math.min(box.w * .16, box.h * .3));
function target() { return document.querySelector<HTMLCanvasElement>('[data-eyes-anchor] canvas'); }
function intro() {
  if (disposed) return;
  const pagePending = document.querySelector('[data-boot-pending="true"]');
  const anchor = document.querySelector('[data-eyes-anchor]');
  const source = target();
  // An unresolved device boot is not a page without eyes. Keep the opaque sky.
  if (!loading.bootDone || pagePending || (anchor && (!source || !source.width))) {
    frame = requestAnimationFrame(intro); return;
  }
  const r = source?.getBoundingClientRect();
  const viewport = host.value!.getBoundingClientRect();
  box.w = r?.width ? r.width * .62 : Math.min(620, viewport.width * .8);
  box.h = r?.height ? r.height * .62 : viewport.height * .32;
  box.x = (viewport.width - box.w) / 2;
  box.y = (viewport.height - box.h) / 2 - viewport.height * .06;
  phase.value = 'open';
  void nextTick(() => {
    if (disposed || !canvas.value) return;
    // Every route uses the same opening engine and the same state timeline.
    // Destination appearance is inherited without following its live expression yet.
    engine = new EyeEngine(canvas.value, { background: 'transparent', attachPointer: false });
    const live = source ? EyeEngine.forCanvas(source) : undefined;
    if (live) {
      engine.setIris(live.irisHexL, live.irisHexR);
      engine.setLight(live.lightCur);
    }
    engine.setBare(1); engine.setZzzEnabled(false); engine.startAsleep(); engine.start();
    later(reduced ? 40 : 400, () => {
      engine?.setMode('sleepy');
      later(reduced ? 30 : 900, () => engine?.setMode('idle'));
    });
    later(reduced ? 100 : 1600, () => {
      tween(reduced ? 20 : 500, p => { bareEyes = 1 - p; engine?.setBare(bareEyes); });
      phase.value = 'awake';
      later(reduced ? 400 : 950, depart);
    });
  });
}
function depart() {
  // The greeting is complete; only now follow changing device state.
  introPose = engine?.capturePresentation();
  tween(reduced ? 20 : 700, p => { liveMix = p; });
  const source = target();
  const a = source?.getBoundingClientRect();
  noAnchor.value = !(a && a.width > 0 && a.height > 0);
  engine?.stop();
  if (a && !noAnchor.value) {
    flight.value = `translate(${a.left - box.x}px, ${a.top - box.y}px) scale(${a.width / box.w}, ${a.height / box.h})`;
    const mirror = () => {
      if (disposed || gone.value || !canvas.value) return;
      const current = target();
      const live = current ? EyeEngine.forCanvas(current) : undefined;
      if (live) live.copyPresentationFrame(canvas.value, 0, 0, introPose, liveMix);
      else if (current) {
        canvas.value.width = current.width; canvas.value.height = current.height;
        canvas.value.getContext('2d')?.drawImage(current, 0, 0);
      }
      mirrorFrame = requestAnimationFrame(mirror);
    };
    mirror();

  }
  phase.value = 'fly';
  loading.bootOverlayVisible = false;
  later(reduced ? 30 : 1000, () => {
    phase.value = 'land';
    document.documentElement.classList.remove('nb-eyes-handoff');
    gone.value = true;
    cancelAnimationFrame(frame); cancelAnimationFrame(mirrorFrame);
    engine?.destroy(); engine = null;
  });
}
onMounted(() => {
  document.documentElement.classList.add('nb-eyes-handoff');
  frame = requestAnimationFrame(intro);
});
onBeforeUnmount(() => {
  disposed = true; timers.forEach(clearTimeout); cancelAnimationFrame(frame); cancelAnimationFrame(mirrorFrame);
  engine?.destroy(); document.documentElement.classList.remove('nb-eyes-handoff');
});
</script>
<template>
  <div v-if="!gone" ref="host" class="nb-boot" :class="[phase, { 'no-anchor': noAnchor }]" aria-live="polite">
    <div class="sky"><i class="b1" /><i class="b2" /><i class="b3" /></div>
    <div class="paper" />
    <div class="night">
      <i v-for="(s, i) in STARS" :key="i" :style="{ left: s.x + '%', top: s.y + '%', width: s.s + 'px', height: s.s + 'px', animationDelay: s.d + 's', '--o': s.o }" />
    </div>
    <div v-if="phase !== 'dusk'" class="eyes" :style="{ left: box.x + 'px', top: box.y + 'px', width: box.w + 'px', height: box.h + 'px', transform: flight || undefined }">
      <div class="halo" />
      <canvas ref="canvas" class="eye-canvas" />
    </div>
    <div class="status" :style="{ top: eyeBottom + 'px' }">
      <div class="swap line-box"><p v-if="phase === 'awake' || phase === 'fly'" class="line">你好呀</p></div>
    </div>
  </div>
</template>

<style scoped>
.nb-boot { position: fixed; inset: 0; z-index: 9999; container-type: size; overflow: hidden; }
/* Stack (bottom -> top): the app's sky | the browser's white page | night. */
.sky { position: absolute; inset: 0; background: radial-gradient(140% 100% at 50% -10%, #0f1630 0%, #070913 45%, #04050a 100%); transition: opacity 0.7s ease 0.1s; }
.sky i { position: absolute; border-radius: 50%; filter: blur(9cqmin); opacity: 0.4; transform: scale(0.8); transition: transform 1.4s cubic-bezier(0.22, 1, 0.36, 1); }
.b1 { width: 60cqw; height: 60cqw; left: -15cqw; top: -30cqw; background: radial-gradient(circle, color-mix(in srgb, rgb(var(--md-primary-rgb)) 55%, #0a1a2a) 0%, transparent 65%); }
.b2 { width: 55cqw; height: 55cqw; right: -18cqw; top: 10cqh; background: radial-gradient(circle, #3b2f7a 0%, transparent 65%); }
.b3 { width: 45cqw; height: 45cqw; left: 25cqw; bottom: -30cqw; background: radial-gradient(circle, #5a2b4f 0%, transparent 65%); opacity: 0.28; }
:root[data-mode='light'] .sky { background: radial-gradient(140% 100% at 50% -10%, #ffffff 0%, #f1f3fb 45%, #e6e9f6 100%); }
:root[data-mode='light'] .sky i { opacity: 0.55; }
:root[data-mode='light'] .b1 { background: radial-gradient(circle, color-mix(in srgb, rgb(var(--md-primary-rgb)) 60%, #ffffff) 0%, transparent 65%); }
:root[data-mode='light'] .b2 { background: radial-gradient(circle, #c9c0ff 0%, transparent 65%); }
:root[data-mode='light'] .b3 { background: radial-gradient(circle, #ffc6da 0%, transparent 65%); opacity: 0.5; }
.awake .sky i, .fly .sky i, .land .sky i { transform: none; }
.paper { position: absolute; inset: 0; background: #fff; }
.open .paper, .load .paper, .awake .paper, .fly .paper, .land .paper { opacity: 0; }
.night { position: absolute; inset: 0; background: radial-gradient(120% 90% at 50% 0%, #0a0d1e 0%, #030409 60%, #010103 100%); opacity: 0; transition: opacity 0.75s cubic-bezier(0.4, 0, 0.2, 1); }
:root[data-mode='light'] .night { background: radial-gradient(120% 90% at 50% 0%, #ffffff 0%, #f7f8fc 60%, #eef0f8 100%); }
:root[data-mode='light'] .night i { display: none; }
.dusk .night, .open .night, .load .night { opacity: 1; }
.night i { position: absolute; border-radius: 50%; background: #fff; opacity: var(--o); animation: nb-tw 4.5s ease-in-out infinite; }
@keyframes nb-tw { 50% { opacity: calc(var(--o) + 0.45); } }
.awake .night { opacity: 0; transition-duration: 1.1s; }
.fly .night, .land .night, .fly .sky, .land .sky { opacity: 0; }

.eyes { position: absolute; transform-origin: 0 0; transition: transform 0.95s cubic-bezier(0.65, 0, 0.25, 1), opacity 0.25s ease; animation: nb-in 0.9s cubic-bezier(0.22, 1, 0.36, 1) backwards; }
@keyframes nb-in { from { opacity: 0; filter: blur(8px); } }
.eye-canvas { position: absolute; inset: 0; width: 100%; height: 100%; display: block; }
.halo { position: absolute; inset: 8% 18%; border-radius: 50%; background: radial-gradient(closest-side, rgba(var(--md-primary-rgb), 0.22), transparent); filter: blur(3cqmin); opacity: 0.2; transition: opacity 0.9s; }
.load .halo, .awake .halo { opacity: 1; }
.fly .halo, .land .halo { opacity: 0; }

/* Text centred between the eyes and the step marks. */
.status { position: absolute; left: 0; right: 0; padding-top: 4cqh; display: flex; flex-direction: column; align-items: center; gap: 0.6cqh; opacity: 0; transform: translateY(1.2cqh); transition: opacity 0.5s ease, transform 0.6s cubic-bezier(0.22, 1, 0.36, 1); }
.load .status, .awake .status { opacity: 1; transform: none; }
.swap { position: relative; width: 100%; display: grid; justify-items: center; }
.swap > * { grid-area: 1 / 1; }
.line-box { min-height: 4cqmin; }
.label-box { min-height: 2.4cqmin; }
.line { margin: 0; font: 400 3.2cqmin/1.2 var(--font-title); letter-spacing: 0.06em; color: var(--md-on-surface); }
.label { margin: 0.4cqh 0 0; font: 600 1.7cqmin var(--font-body); letter-spacing: 0.32em; padding-left: 0.32em; color: var(--md-on-surface-variant); }
.steps { display: flex; gap: 1cqmin; margin-top: 3.4cqh; transition: opacity 0.3s; }
.steps i { width: 3.6cqmin; height: 3px; border-radius: 3px; background: rgba(var(--md-primary-rgb), 0.16); transition: background 0.4s, box-shadow 0.4s; }
.steps i.on { background: var(--md-primary); box-shadow: 0 0 1cqmin rgba(var(--md-primary-rgb), 0.8); }
.awake .label-box, .awake .steps { opacity: 0; transition: opacity 0.3s; }
.fly .status, .land .status { opacity: 0; transform: translateY(1cqh); }
.land .eyes { opacity: 1; }
.no-anchor.fly .eyes { opacity: 0; transition: opacity 0.65s cubic-bezier(0.4, 0, 0.2, 1); }
.nb-xf-enter-active { transition: opacity 0.6s ease 0.08s, transform 0.7s cubic-bezier(0.22, 1, 0.36, 1) 0.08s, filter 0.6s ease 0.08s; }
.nb-xf-leave-active { transition: opacity 0.5s ease, transform 0.6s cubic-bezier(0.4, 0, 0.6, 1), filter 0.5s ease; }
.nb-xf-enter-from { opacity: 0; transform: translateY(1.2cqh); filter: blur(5px); }
.nb-xf-leave-to { opacity: 0; transform: translateY(-1.2cqh); filter: blur(5px); }
@media (prefers-reduced-motion: reduce) {
  .eyes, .night, .sky, .status { transition-duration: 0.01ms !important; }
}
</style>
