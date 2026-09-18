<script setup lang="ts">
/* Tablet: single column feature body; the eye preview sits below the
 * feature so it stays reachable without a context panel. */
import { computed, inject } from 'vue';
import type { useFormFactor } from '../../composables/useFormFactor';
import ServiceDetailFrame from './ServiceDetailFrame.vue';
import ServiceEyePreview from './ServiceEyePreview.vue';
import { useServiceDetail } from './serviceDetail.logic';

const props = defineProps<{ key?: string; type?: string }>();
const type = computed(() => props.type ?? '');
const detail = useServiceDetail(type);
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
</script>

<template>
  <div class="page detail-tablet">
    <ServiceDetailFrame :detail="detail" :type="type" :ff="ff.formFactor.value" />
    <ServiceEyePreview v-if="detail.def.value" :detail="detail" height="180px" />
  </div>
</template>

<style scoped>
.detail-tablet { display: flex; flex-direction: column; gap: 18px; width: 100%; max-width: 760px; margin: 0 auto; }
</style>
