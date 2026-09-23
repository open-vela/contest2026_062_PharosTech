<script setup lang="ts">
/* Frame picker for bare content that is the same on every form factor (the
 * boot view). Real pages pick their frame in their own variants.
 * (LayoutSwitch cannot be used here: it does not forward slots.) */
import { computed, inject } from 'vue';
import type { useFormFactor } from '../../composables/useFormFactor';
import GateDesktop from './GateFrame.desktop.vue';
import GateTablet from './GateFrame.tablet.vue';
import GatePhone from './GateFrame.phone.vue';

defineProps<{ title: string; sub?: string; wide?: boolean }>();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor');
const frame = computed(() => (ff?.formFactor.value === 'phone' ? GatePhone : ff?.formFactor.value === 'tablet' ? GateTablet : GateDesktop));
</script>

<template>
  <component :is="frame" :title="title" :sub="sub" :wide="wide"><slot /></component>
</template>
