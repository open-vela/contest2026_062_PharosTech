<script setup lang="ts">
/* Desktop: centered feature body (max 880px) with the eye preview in the
 * shell's context panel. */
import { computed, inject } from 'vue';
import ContextSlot from '../../components/ContextSlot.vue';
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
  <div class="page detail-desktop">
    <div class="body">
      <ServiceDetailFrame :detail="detail" :type="type" :ff="ff.formFactor.value" />
    </div>
    <ContextSlot>
      <ServiceEyePreview :detail="detail" height="220px" />
    </ContextSlot>
  </div>
</template>

<style scoped>
.detail-desktop { display: flex; flex-direction: column; align-items: center; }
.body { width: 100%; max-width: 880px; }
</style>
