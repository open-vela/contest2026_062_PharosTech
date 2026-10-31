<script setup lang="ts">
/* First-paint overlay: renders the boot overlay of the active skin. The
 * skin is fixed for the lifetime of the overlay (boot happens once). */
import { useThemeStore } from '../stores/theme';
import { SKIN_BOOT } from '../skins';

const boot = SKIN_BOOT[useThemeStore().skin];
</script>

<template>
  <component :is="boot" />
</template>
