<script setup lang="ts">
/* Slow drifting aurora blobs tinted by the palette (primary / tertiary),
 * plus a faint starfield in dark mode only. Fixed behind the shell. */
const STARS = Array.from({ length: 70 }, (_, i) => ({
  // Hash-scattered (a golden-angle lattice lines up into columns).
  x: (((Math.sin(i * 12.9898 + 1.7) * 43758.5453) % 1) + 1) % 1 * 100,
  y: (((Math.sin(i * 78.233 + 4.1) * 24634.6345) % 1) + 1) % 1 * 100,
  s: i % 9 === 0 ? 2 : 1,
  d: (i % 11) * 0.6,
}));
</script>

<template>
  <div class="aurora" aria-hidden="true">
    <div class="blob b1" />
    <div class="blob b2" />
    <div class="blob b3" />
    <div class="stars">
      <i v-for="(s, i) in STARS" :key="i" :style="{ left: s.x + '%', top: s.y + '%', width: s.s + 'px', height: s.s + 'px', animationDelay: s.d + 's' }" />
    </div>
    <div class="vignette" />
  </div>
</template>

<style scoped>
.aurora {
  position: fixed;
  inset: 0;
  z-index: -1;
  overflow: hidden;
  pointer-events: none;
  background: radial-gradient(140% 100% at 50% -10%, color-mix(in srgb, var(--md-surface-container-highest) 70%, var(--md-surface)) 0%, var(--md-surface) 50%, var(--md-surface-dim) 100%);
}
.blob { position: absolute; border-radius: 50%; filter: blur(90px); opacity: 0.32; will-change: transform; }
.b1 { width: 60vw; height: 60vw; left: -15vw; top: -25vw; background: radial-gradient(circle, var(--md-primary) 0%, transparent 62%); animation: drift1 38s ease-in-out infinite alternate; }
.b2 { width: 55vw; height: 55vw; right: -18vw; top: 10vh; background: radial-gradient(circle, var(--md-tertiary) 0%, transparent 62%); animation: drift2 46s ease-in-out infinite alternate; }
.b3 { width: 45vw; height: 45vw; left: 25vw; bottom: -30vw; background: radial-gradient(circle, color-mix(in srgb, var(--md-tertiary) 50%, #ff8fb1) 0%, transparent 62%); opacity: 0.2; animation: drift3 52s ease-in-out infinite alternate; }
:root[data-mode='light'] .blob { opacity: 0.2; }
:root[data-mode='light'] .b3 { opacity: 0.12; }
@keyframes drift1 { to { transform: translate3d(12vw, 10vh, 0) scale(1.15); } }
@keyframes drift2 { to { transform: translate3d(-14vw, 14vh, 0) scale(0.9); } }
@keyframes drift3 { to { transform: translate3d(-10vw, -12vh, 0) scale(1.2); } }
.stars i { position: absolute; border-radius: 50%; background: #fff; opacity: 0.25; animation: tw 5s ease-in-out infinite; }
:root[data-mode='light'] .stars { display: none; }
@keyframes tw { 50% { opacity: 0.75; } }
.vignette { position: absolute; inset: 0; background: radial-gradient(120% 90% at 50% 40%, transparent 50%, rgba(0, 0, 0, 0.45) 100%); }
:root[data-mode='light'] .vignette { background: radial-gradient(120% 90% at 50% 40%, transparent 55%, rgba(80, 90, 150, 0.1) 100%); }
@media (prefers-reduced-motion: reduce) { .blob, .stars i { animation: none; } }
</style>
