<script setup lang="ts">
/* Material icon renderer backed by the eye-engine path set. Unknown names fall
 * back to a neutral dot so plugin trees never break on missing glyphs. */
import { computed } from 'vue';
import { MATERIAL_ICONS } from '@nyabula/eye-engine';

const props = defineProps<{ name?: string; size?: number }>();

const icon = computed(() => (props.name ? MATERIAL_ICONS[props.name] : undefined));
const px = computed(() => props.size ?? 24);
</script>

<template>
  <svg
    v-if="icon"
    class="nya-icon"
    :width="px"
    :height="px"
    :viewBox="icon.viewBox.join(' ')"
    aria-hidden="true"
  >
    <path :d="icon.path" fill="currentColor" />
  </svg>
  <svg v-else class="nya-icon" :width="px" :height="px" viewBox="0 0 24 24" aria-hidden="true">
    <circle cx="12" cy="12" r="5" fill="currentColor" opacity="0.35" />
  </svg>
</template>

<style scoped>
.nya-icon {
  display: inline-block;
  vertical-align: middle;
  flex: none;
}
</style>
