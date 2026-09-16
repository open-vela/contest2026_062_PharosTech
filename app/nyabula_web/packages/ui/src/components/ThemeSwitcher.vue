<script setup lang="ts">
/* Palette dots + light/dark toggle with circular reveal (ported from Myself).
 * `compact` hides the palette dots (round mode button only). */
import { useThemeStore } from '../stores/theme';
import { circularReveal, eventOrigin } from '../theme/circularReveal';
import UiIcon from './UiIcon.vue';

defineProps<{ compact?: boolean }>();
const theme = useThemeStore();

function onPalette(e: MouseEvent, id: string): void {
  if (id === theme.paletteId) return;
  circularReveal(eventOrigin(e), () => theme.setPalette(id), 'expand');
}
function onMode(e: MouseEvent): void {
  const toLight = theme.mode === 'dark';
  circularReveal(eventOrigin(e), () => theme.toggleMode(), toLight ? 'expand' : 'contract');
}
</script>

<template>
  <div class="switcher" :class="{ solo: compact }">
    <template v-if="!compact">
      <button
        v-for="p in theme.palettes"
        :key="p.id"
        class="dot"
        :class="{ active: theme.paletteId === p.id }"
        :style="{ background: p[theme.mode].primary }"
        :title="p.name"
        :aria-label="p.name"
        @click="onPalette($event, p.id)"
      />
      <span class="divider" />
    </template>
    <button class="mode-btn" :title="theme.mode === 'light' ? '切换深色' : '切换浅色'" @click="onMode($event)">
      <UiIcon :name="theme.mode === 'light' ? 'light_mode' : 'dark_mode'" :size="19" />
    </button>
  </div>
</template>

<style scoped>
.switcher {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 10px;
  border-radius: 999px;
  background: var(--md-glass);
  backdrop-filter: blur(14px) saturate(1.5);
  -webkit-backdrop-filter: blur(14px) saturate(1.5);
  border: 1px solid rgba(var(--md-primary-rgb), 0.18);
  box-shadow: var(--md-elev-1), inset 0 1px 0 rgba(255, 255, 255, 0.08);
}
.switcher.solo {
  width: 40px;
  height: 40px;
  padding: 0;
  justify-content: center;
}
.dot {
  width: 16px;
  height: 16px;
  border-radius: 50%;
  border: 2px solid transparent;
  cursor: pointer;
  padding: 0;
  transition: transform var(--dur-fast) var(--ease-spring), border-color var(--dur-fast), box-shadow var(--dur-fast);
}
.dot:hover { transform: scale(1.2); }
.dot.active {
  border-color: var(--md-surface);
  transform: scale(1.25);
  box-shadow: 0 0 0 1.5px rgba(var(--md-primary-rgb), 0.7), 0 0 10px rgba(var(--md-primary-rgb), 0.5);
}
.divider { width: 1px; height: 16px; background: var(--md-outline-variant); }
.mode-btn {
  border: none;
  background: none;
  width: 26px;
  height: 26px;
  display: grid;
  place-items: center;
  color: var(--md-on-surface);
  cursor: pointer;
  padding: 0;
  transition: transform var(--dur-fast) var(--ease-spring);
}
.mode-btn:hover { transform: rotate(18deg) scale(1.12); }
</style>
