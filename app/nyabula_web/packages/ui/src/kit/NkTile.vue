<script setup lang="ts">
/* Device-card style tile. `layout` square stacks icon/title vertically;
 * `wide` lays them out horizontally. */
import UiIcon from '../components/UiIcon.vue';

export interface NkTileProps {
  icon?: string;
  title: string;
  sub?: string;
  value?: string | number;
  active?: boolean;
  layout?: 'square' | 'wide';
  disabled?: boolean;
}
const props = withDefaults(defineProps<NkTileProps>(), { layout: 'square', active: false, disabled: false });
const emit = defineEmits<{ (e: 'tap'): void }>();

function onTap(): void {
  if (!props.disabled) emit('tap');
}
</script>

<template>
  <button
    type="button"
    class="nk-tile"
    :class="[layout, { active, disabled }]"
    :disabled="disabled"
    :aria-pressed="active"
    @click="onTap"
  >
    <span v-if="icon" class="nk-tile-icon"><UiIcon :name="icon" :size="24" /></span>
    <span class="nk-tile-body">
      <span class="nk-tile-title">{{ title }}</span>
      <span v-if="sub" class="nk-tile-sub">{{ sub }}</span>
    </span>
    <span v-if="value !== undefined && value !== ''" class="nk-tile-value">{{ value }}</span>
  </button>
</template>

<style scoped>
.nk-tile {
  position: relative;
  display: flex;
  gap: 10px;
  min-height: 44px;
  width: 100%;
  padding: 14px;
  border: none;
  border-radius: var(--radius-m);
  background: var(--md-surface-container-high);
  color: var(--md-on-surface);
  text-align: left;
  cursor: pointer;
  font-family: var(--font-body);
  transition: background var(--dur-fast), transform var(--dur-fast), color var(--dur-fast);
  -webkit-tap-highlight-color: transparent;
}
.nk-tile:active:not(.disabled) { transform: scale(0.98); }
.nk-tile.square { flex-direction: column; align-items: flex-start; aspect-ratio: 1 / 1; justify-content: space-between; }
.nk-tile.wide { flex-direction: row; align-items: center; }
.nk-tile-icon {
  width: 40px; height: 40px; flex: none;
  display: grid; place-items: center;
  border-radius: var(--radius-full);
  background: var(--md-surface-container-highest);
  color: var(--md-on-surface-variant);
  transition: background var(--dur-fast), color var(--dur-fast);
}
.nk-tile-body { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
.nk-tile-title { font-weight: 600; font-size: 14px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.nk-tile-sub { font-size: 12px; color: var(--md-on-surface-variant); }
.nk-tile-value { font: 600 15px var(--font-title); color: var(--md-on-surface-variant); }
.nk-tile.active { background: var(--md-primary-container); color: var(--md-on-primary-container); }
.nk-tile.active .nk-tile-icon { background: var(--md-primary); color: var(--md-on-primary); }
.nk-tile.active .nk-tile-sub, .nk-tile.active .nk-tile-value { color: var(--md-on-primary-container); opacity: 0.85; }
.nk-tile.disabled { opacity: 0.45; cursor: default; }
.nk-tile:focus-visible { outline: 2px solid var(--md-primary); outline-offset: 2px; }
</style>
