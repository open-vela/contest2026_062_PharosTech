<script setup lang="ts">
/* "Show on the device" switch shared by the feature cards and pages. It only
 * reflects and toggles; the owning feature builds the payload (useEyeScene). */
import { MdButton, UiIcon } from '@nyabula/ui';

withDefaults(defineProps<{
  /** The scene is on the device (shown or held by this panel). */
  shown: boolean;
  /** Showing is not possible right now (no data, no control). Hiding always is. */
  disabled?: boolean;
  /** round: icon only, for dense cards; pill: short text; wide: full sentence. */
  kind?: 'round' | 'pill' | 'wide';
}>(), { disabled: false, kind: 'pill' });
const emit = defineEmits<{ (e: 'toggle'): void }>();
</script>

<template>
  <button
    v-if="kind === 'round'"
    type="button"
    class="eb"
    :class="{ on: shown }"
    :disabled="disabled && !shown"
    :aria-pressed="shown"
    :aria-label="shown ? '从设备上隐藏' : '在设备上显示'"
    :title="shown ? '从设备上隐藏' : '在设备上显示'"
    @click="emit('toggle')"
  >
    <UiIcon name="visibility" :size="20" />
  </button>
  <MdButton
    v-else
    :variant="shown ? (kind === 'wide' ? 'outlined' : 'text') : 'tonal'"
    class="eb-text"
    :class="kind"
    :disabled="disabled && !shown"
    @click="emit('toggle')"
  >
    <UiIcon v-if="kind === 'wide'" :name="shown ? 'close' : 'visibility'" :size="18" />
    {{ kind === 'wide' ? (shown ? '退出显示' : '在设备上显示') : (shown ? '隐藏' : '显示') }}
  </MdButton>
</template>

<style scoped>
.eb {
  width: 40px; height: 40px; border-radius: 50%; border: none; cursor: pointer; flex: none;
  display: grid; place-items: center; background: transparent; color: var(--md-on-surface-variant);
}
.eb:hover:not(:disabled) { background: var(--md-surface-container-highest); color: var(--md-on-surface); }
.eb.on { background: var(--md-primary-container); color: var(--md-on-primary-container); }
.eb:disabled { opacity: 0.35; cursor: default; }
.eb-text { flex: none; min-height: 40px; padding: 0 16px; }
.eb-text.wide { display: inline-flex; align-items: center; justify-content: center; gap: 6px; min-height: 44px; }
</style>
