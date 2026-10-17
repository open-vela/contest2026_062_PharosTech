<script setup lang="ts">
/* Phone: single column feature body only; the eye is visible on the home
 * page, so no preview here to keep the controls above the fold. */
import { computed, inject } from 'vue';
import type { useFormFactor } from '../../composables/useFormFactor';
import ServiceDetailFrame from './ServiceDetailFrame.vue';
import { useServiceDetail } from './serviceDetail.logic';

const props = defineProps<{ key?: string; type?: string }>();
const type = computed(() => props.type ?? '');
const detail = useServiceDetail(type);
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
</script>

<template>
  <div class="page narrow detail-phone">
    <ServiceDetailFrame :detail="detail" :type="type" :ff="ff.formFactor.value" />
  </div>
</template>

<style scoped>
.detail-phone { display: flex; flex-direction: column; padding-bottom: calc(var(--shell-bottom, 0px) + 24px); }
</style>
