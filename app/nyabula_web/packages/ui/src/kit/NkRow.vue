<script setup lang="ts">
/* Settings row container: leading icon, title, subtitle, trailing slot.
 * Emits `tap` when `tappable` (renders as a button with chevron). */
import UiIcon from '../components/UiIcon.vue';

export interface NkRowProps {
  icon?: string;
  title: string;
  sub?: string;
  tappable?: boolean;
  disabled?: boolean;
}
const props = withDefaults(defineProps<NkRowProps>(), { tappable: false, disabled: false });
const emit = defineEmits<{ (e: 'tap'): void }>();
function onTap(): void {
  if (props.tappable && !props.disabled) emit('tap');
}
</script>

<template>
  <div
    class="nk-row"
    :class="{ tappable, disabled }"
    :role="tappable ? 'button' : undefined"
    :tabindex="tappable && !disabled ? 0 : undefined"
    @click="onTap"
    @keydown.enter.prevent="onTap"
    @keydown.space.prevent="onTap"
  >
    <span v-if="icon" class="nk-row-icon"><UiIcon :name="icon" :size="22" /></span>
    <div class="nk-row-main">
      <div class="nk-row-title">{{ title }}</div>
      <div v-if="sub" class="nk-row-sub">{{ sub }}</div>
    </div>
    <div class="nk-row-trail">
      <slot />
      <UiIcon v-if="tappable && !$slots.default" name="chevron_right" :size="20" class="nk-row-chev" />
    </div>
  </div>
</template>

<style scoped>
.nk-row {
  display: flex; align-items: center; gap: 12px;
  min-height: 52px; padding: 8px 12px;
  border-radius: var(--radius-m);
  color: var(--md-on-surface);
  transition: background var(--dur-fast);
}
.nk-row.tappable { cursor: pointer; }
.nk-row.tappable:hover, .nk-row.tappable:focus-visible { background: var(--md-surface-container-high); outline: none; }
.nk-row.disabled { opacity: 0.45; pointer-events: none; }
.nk-row-icon { width: 36px; height: 36px; flex: none; display: grid; place-items: center; border-radius: var(--radius-full); background: var(--md-surface-container-highest); color: var(--md-on-surface-variant); }
.nk-row-main { flex: 1; min-width: 0; }
.nk-row-title { font: 600 14px var(--font-body); }
.nk-row-sub { margin-top: 2px; font: 400 12px var(--font-body); color: var(--md-on-surface-variant); }
.nk-row-trail { display: flex; align-items: center; gap: 8px; flex: none; color: var(--md-on-surface-variant); }
</style>
