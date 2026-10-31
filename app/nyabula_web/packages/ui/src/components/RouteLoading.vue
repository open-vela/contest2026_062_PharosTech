<script setup lang="ts">
/* Route curtain: renders the curtain of the active skin. */
import { computed } from 'vue';
import { useThemeStore } from '../stores/theme';
import { SKIN_CURTAIN } from '../skins';

const theme = useThemeStore();
const curtain = computed(() => SKIN_CURTAIN[theme.skin]);
</script>

<template>
  <component :is="curtain" />
</template>
