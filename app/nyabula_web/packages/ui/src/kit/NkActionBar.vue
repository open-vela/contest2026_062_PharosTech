<script setup lang="ts">
/* Primary + secondary action bar. `fixed` pins it to the bottom of the
 * viewport above the phone shell tab bar (var(--shell-bottom) + safe area). */
import MdButton from '../components/MdButton.vue';
import UiIcon from '../components/UiIcon.vue';

export interface NkActionBarProps {
  primaryText: string;
  primaryIcon?: string;
  secondaryText?: string;
  secondaryIcon?: string;
  fixed?: boolean;
  disabled?: boolean;
  busy?: boolean;
  danger?: boolean;
}
withDefaults(defineProps<NkActionBarProps>(), { fixed: false, disabled: false, busy: false, danger: false });
const emit = defineEmits<{ (e: 'primary'): void; (e: 'secondary'): void }>();
</script>

<template>
  <div class="nk-actionbar" :class="{ fixed, danger }">
    <MdButton v-if="secondaryText" variant="tonal" class="nk-action secondary" :disabled="disabled || busy" @click="emit('secondary')">
      <span class="nk-action-inner"><UiIcon v-if="secondaryIcon" :name="secondaryIcon" :size="18" />{{ secondaryText }}</span>
    </MdButton>
    <MdButton variant="filled" class="nk-action primary" :disabled="disabled || busy" @click="emit('primary')">
      <span class="nk-action-inner"><UiIcon v-if="primaryIcon" :name="primaryIcon" :size="18" />{{ busy ? '处理中…' : primaryText }}</span>
    </MdButton>
  </div>
</template>

<style scoped>
.nk-actionbar { display: flex; gap: 10px; align-items: center; }
.nk-actionbar.fixed {
  position: fixed; left: 0; right: 0;
  bottom: calc(var(--shell-bottom, 0px) + var(--safe-b, 0px));
  padding: 10px 14px 12px;
  background: var(--md-glass);
  backdrop-filter: blur(16px);
  -webkit-backdrop-filter: blur(16px);
  border-top: 1px solid var(--md-outline-variant);
  z-index: 20;
}
.nk-action { min-height: 48px; flex: 1; }
.nk-action.secondary { flex: 0 0 auto; }
.nk-action-inner { display: inline-flex; align-items: center; justify-content: center; gap: 6px; }
.nk-actionbar.danger .nk-action.primary { background: var(--md-error); color: var(--md-on-error); }
</style>
