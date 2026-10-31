<script setup lang="ts">
/* Page entry that honours the active skin: a skin-provided page component
 * wins; otherwise the classic per-form-factor variants via LayoutSwitch. */
import { computed, type Component } from 'vue';
import { useThemeStore } from '@nyabula/ui';
import LayoutSwitch from './LayoutSwitch.vue';
import { APP_SKINS } from '../skins/registry';
import type { SkinPageId } from '../skins/types';

const props = defineProps<{ page: SkinPageId; desktop?: Component; tablet?: Component; phone?: Component }>();
const theme = useThemeStore();
const skinned = computed(() => APP_SKINS[theme.skin].pages?.[props.page]);
</script>

<template>
  <component :is="skinned" v-if="skinned" v-bind="$attrs" />
  <LayoutSwitch v-else :desktop="desktop" :tablet="tablet" :phone="phone" v-bind="$attrs" />
</template>
