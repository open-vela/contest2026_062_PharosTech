<script setup lang="ts">
/* Dot-matrix readout. Each glyph cell keeps a stable key, so when the text
 * changes only the dots that flip animate — with a per-row/column delay the
 * refresh sweeps across the glyph like a real LED panel. */
import { computed } from 'vue';
import { GLYPHS } from './glyphs';

const props = withDefaults(defineProps<{ text: string | number; dot?: number; gap?: number; color?: string; off?: number }>(), {
  dot: 4,
  gap: 1.5,
  color: 'currentColor',
  off: 0.08,
});

interface Dot { k: string; cx: number; cy: number; on: boolean; d: number }

const layout = computed(() => {
  const pitch = props.dot + props.gap;
  const chars = String(props.text).toUpperCase().split('');
  const dots: Dot[] = [];
  let col = 0;
  chars.forEach((ch, gi) => {
    const g = GLYPHS[ch] ?? GLYPHS[' '];
    const w = g[0].length;
    for (let r = 0; r < 7; r++) {
      for (let c = 0; c < w; c++) {
        dots.push({
          k: `${gi}-${r}-${c}`,
          cx: (col + c) * pitch + props.dot / 2,
          cy: r * pitch + props.dot / 2,
          on: g[r][c] === '#',
          d: r * 18 + c * 7,
        });
      }
    }
    col += w + 1;
  });
  const cols = Math.max(0, col - 1);
  return { dots, w: cols * pitch - props.gap, h: 7 * pitch - props.gap };
});
</script>

<template>
  <svg
    class="dm"
    :width="layout.w"
    :height="layout.h"
    :viewBox="`0 0 ${layout.w} ${layout.h}`"
    :style="{ color }"
    role="img"
    :aria-label="String(text)"
  >
    <circle
      v-for="p in layout.dots"
      :key="p.k"
      :cx="p.cx"
      :cy="p.cy"
      :r="dot / 2"
      :class="{ on: p.on }"
      :style="{ transitionDelay: p.d + 'ms', '--off': off }"
    />
  </svg>
</template>

<style scoped>
.dm { display: block; overflow: visible; flex: none; }
circle {
  fill: currentColor;
  opacity: var(--off);
  transition: opacity 90ms steps(2, jump-none);
}
circle.on { opacity: 1; }
</style>
