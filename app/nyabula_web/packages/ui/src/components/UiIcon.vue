<script setup lang="ts">
/* Icon renderer: UI_ICONS (24px Material-style) first, then the eye-engine
 * scene icon table (its own viewBox), else a neutral dot so nothing breaks. */
import { computed } from 'vue';
import { MATERIAL_ICONS } from '@nyabula/eye-engine';
import { UI_ICONS } from '../icons';

const props = defineProps<{ name?: string; size?: number | string }>();

const px = computed(() => (typeof props.size === 'number' ? `${props.size}px` : props.size ?? '24px'));
const ui = computed(() => (props.name ? UI_ICONS[props.name] : undefined));
const scene = computed(() => (!ui.value && props.name ? MATERIAL_ICONS[props.name] : undefined));
</script>

<template>
  <svg v-if="ui" class="ui-icon" :style="{ width: px, height: px }" viewBox="0 0 24 24" aria-hidden="true">
    <path :d="ui" fill="currentColor" />
  </svg>
  <svg
    v-else-if="scene"
    class="ui-icon"
    :style="{ width: px, height: px }"
    :viewBox="scene.viewBox.join(' ')"
    aria-hidden="true"
  >
    <path :d="scene.path" fill="currentColor" />
  </svg>
  <svg v-else class="ui-icon" :style="{ width: px, height: px }" viewBox="0 0 24 24" aria-hidden="true">
    <circle cx="12" cy="12" r="5" fill="currentColor" opacity="0.35" />
  </svg>
</template>

<style scoped>
.ui-icon {
  display: inline-block;
  vertical-align: middle;
  flex: none;
}
</style>
