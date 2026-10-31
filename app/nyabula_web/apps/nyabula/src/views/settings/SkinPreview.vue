<script setup lang="ts">
/* Miniature of a skin's look for the skin picker. Colors are fixed per
 * skin (independent of the active one) except the accent, which follows
 * the current palette so the preview shows what you will get. */
import type { ThemeSkin } from '@nyabula/ui';

defineProps<{ skin: ThemeSkin; accent: string; dark: boolean }>();
</script>

<template>
  <div class="pv" :class="[skin, { dark }]" :style="{ '--acc': accent }" aria-hidden="true">
    <template v-if="skin === 'nebula'">
      <span class="aura a1" /><span class="aura a2" />
      <span class="eyes"><i /><i /></span>
      <span class="glass" />
      <span class="dock"><b /><b class="on" /><b /><b /></span>
    </template>
    <template v-else-if="skin === 'classic'">
      <span class="rail"><b class="on" /><b /><b /></span>
      <span class="card c1" /><span class="card c2" /><span class="card c3" />
    </template>
    <template v-else>
      <span class="grid" />
      <span class="keys"><b class="on" /><b /><b /><b /></span>
      <span class="dots"><i v-for="n in 15" :key="n" :class="{ lit: [1, 2, 3, 6, 8, 11, 12, 13].includes(n) }" /></span>
      <span class="led"><i v-for="n in 8" :key="n" :class="{ lit: n < 6 }" /></span>
    </template>
  </div>
</template>

<style scoped>
.pv { position: relative; width: 100%; aspect-ratio: 16 / 10; border-radius: 12px; overflow: hidden; isolation: isolate; }
/* nebula */
.pv.nebula { background: #eef0fa; }
.pv.nebula.dark { background: #070918; }
.aura { position: absolute; border-radius: 50%; filter: blur(14px); opacity: 0.7; }
.a1 { width: 60%; height: 70%; left: -10%; top: -20%; background: var(--acc); opacity: 0.45; }
.a2 { width: 50%; height: 60%; right: -12%; top: 10%; background: #8b7cff; opacity: 0.4; }
.eyes { position: absolute; left: 50%; top: 38%; transform: translate(-50%, -50%); display: flex; gap: 14%; width: 46%; justify-content: center; }
.eyes i { width: 34%; aspect-ratio: 1; border-radius: 50%; background: var(--acc); box-shadow: 0 0 12px var(--acc); }
.glass { position: absolute; left: 12%; right: 12%; bottom: 26%; height: 14%; border-radius: 8px; background: rgba(255, 255, 255, 0.5); border: 1px solid rgba(255, 255, 255, 0.6); }
.dark .glass { background: rgba(40, 48, 90, 0.5); border-color: rgba(255, 255, 255, 0.12); }
.dock { position: absolute; left: 50%; bottom: 7%; transform: translateX(-50%); display: flex; gap: 5px; padding: 4px 6px; border-radius: 99px; background: rgba(255, 255, 255, 0.7); }
.dark .dock { background: rgba(40, 48, 90, 0.7); }
.dock b { width: 7px; height: 7px; border-radius: 50%; background: currentColor; opacity: 0.3; color: #556; }
.dark .dock b { color: #cfd6ff; }
.dock b.on { background: var(--acc); opacity: 1; }
/* classic */
.pv.classic { background: #f3f5f6; }
.pv.classic.dark { background: #111416; }
.rail { position: absolute; left: 0; top: 0; bottom: 0; width: 16%; background: #e6eaec; display: flex; flex-direction: column; align-items: center; gap: 6px; padding-top: 10%; }
.dark .rail { background: #1b1f22; }
.rail b { width: 50%; aspect-ratio: 1.6; border-radius: 99px; background: rgba(127, 127, 127, 0.25); }
.rail b.on { background: var(--acc); opacity: 0.6; }
.card { position: absolute; border-radius: 6px; background: #fff; box-shadow: 0 1px 2px rgba(0, 0, 0, 0.08); }
.dark .card { background: #1f2427; box-shadow: none; }
.c1 { left: 22%; top: 10%; width: 44%; height: 38%; }
.c2 { left: 70%; top: 10%; width: 24%; height: 38%; }
.c3 { left: 22%; top: 54%; width: 72%; height: 36%; }
.c1::after { content: ''; position: absolute; left: 10%; top: 20%; width: 30%; height: 12%; border-radius: 4px; background: var(--acc); opacity: 0.7; }
/* instrument */
.pv.instrument { background: #ecebe6; color: #111; }
.pv.instrument.dark { background: #0b0b0c; color: #e9e9e6; }
.grid { position: absolute; inset: 0; background-image: linear-gradient(currentColor 1px, transparent 1px), linear-gradient(90deg, currentColor 1px, transparent 1px); background-size: 12.5% 20%; opacity: 0.08; }
.keys { position: absolute; left: 8%; right: 8%; top: 9%; display: flex; gap: 4px; }
.keys b { flex: 1; height: 9px; border: 1px solid currentColor; opacity: 0.35; border-radius: 2px; }
.keys b.on { opacity: 1; box-shadow: inset 0 0 0 2px transparent; background: linear-gradient(90deg, var(--acc) 0 22%, transparent 22%); }
.dots { position: absolute; left: 8%; top: 32%; display: grid; grid-template-columns: repeat(3, 7px); gap: 3px; }
.dots i { width: 7px; height: 7px; border-radius: 50%; background: currentColor; opacity: 0.12; }
.dots i.lit { opacity: 1; }
.led { position: absolute; left: 8%; right: 8%; bottom: 12%; display: flex; gap: 3px; }
.led i { flex: 1; height: 7px; background: currentColor; opacity: 0.12; }
.led i.lit { background: var(--acc); opacity: 1; }
</style>
