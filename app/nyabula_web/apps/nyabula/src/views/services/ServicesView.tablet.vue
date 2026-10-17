<script setup lang="ts">
/* Tablet: "now showing" bar + the two stages (已实现 / 规划中), tiles in 2
 * columns landscape and 1 column portrait. */
import { computed, inject } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';
import type { useFormFactor } from '../../composables/useFormFactor';
import { useServicesPage } from './services.logic';
import FeatureSections from './FeatureSections.vue';

defineProps<{ key?: string }>();
const page = useServicesPage();
const { session, activeDef } = page;
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const portrait = computed(() => ff.orientation.value === 'portrait');
</script>

<template>
  <div class="page services-tablet" :class="{ portrait }">
    <h1 class="page-title">功能</h1>
    <div class="now" :class="{ none: !activeDef }">
      <UiIcon :name="activeDef?.icon ?? 'face'" :size="20" />
      <span class="now-text">正在显示：{{ activeDef ? activeDef.label : session.connected ? '无（表情模式）' : '设备离线' }}</span>
      <MdButton v-if="activeDef" variant="tonal" class="exit" :disabled="!session.canControl" @click="page.exit()">退出</MdButton>
    </div>

    <FeatureSections :page="page" :ff="ff.formFactor.value" :columns="portrait ? 'minmax(0, 1fr)' : 'repeat(2, minmax(0, 1fr))'" />
  </div>
</template>

<style scoped>
.services-tablet { display: flex; flex-direction: column; gap: 16px; }
.now {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 10px 14px;
  min-height: 48px;
  border-radius: var(--radius-l);
  background: var(--md-primary-container);
  color: var(--md-on-primary-container);
}
.now.none { background: var(--md-surface-container); color: var(--md-on-surface-variant); }
.now-text { flex: 1; font-weight: 600; font-size: 14px; }
.exit { padding: 8px 14px; font-size: 13px; }
</style>
