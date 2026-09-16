<script setup lang="ts">
/* Picks the page variant for the current form factor. Missing variants fall
 * back to the nearest one: phone <- tablet <- desktop. */
import { computed, inject, type Component } from 'vue';
import type { useFormFactor } from '../composables/useFormFactor';

const props = defineProps<{ desktop?: Component; tablet?: Component; phone?: Component }>();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;

const picked = computed<Component | undefined>(() => {
  const f = ff.formFactor.value;
  if (f === 'desktop') return props.desktop ?? props.tablet ?? props.phone;
  if (f === 'tablet') return props.tablet ?? props.desktop ?? props.phone;
  return props.phone ?? props.tablet ?? props.desktop;
});
</script>

<template>
  <component :is="picked" v-if="picked" v-bind="$attrs" />
</template>
