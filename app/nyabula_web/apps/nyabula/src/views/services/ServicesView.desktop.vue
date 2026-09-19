<script setup lang="ts">
/* Desktop: "now showing" bar, then the two stages (已实现 / 规划中), each
 * group as an auto-filling tile grid. Tiles open the feature page. */
import { MdButton, UiIcon } from '@nyabula/ui';
import { inject } from 'vue';
import type { useFormFactor } from '../../composables/useFormFactor';
import { useServicesPage } from './services.logic';
import FeatureSections from './FeatureSections.vue';

defineProps<{ key?: string }>();
const page = useServicesPage();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const { session, activeDef } = page;
</script>

<template>
  <div class="page services-desktop">
    <div class="row between top">
      <div>
        <h1 class="page-title">功能</h1>
        <p class="page-sub">选择要在设备上显示的功能，点击进入调节。</p>
      </div>
      <div class="now" :class="{ none: !activeDef }">
        <UiIcon :name="activeDef?.icon ?? 'face'" :size="20" />
        <span class="now-text">正在显示：{{ activeDef ? activeDef.label : session.connected ? '无（表情模式）' : '设备离线' }}</span>
        <MdButton v-if="activeDef" variant="tonal" class="exit" :disabled="!session.canControl" @click="page.exit()">退出</MdButton>
      </div>
    </div>

    <FeatureSections :page="page" :ff="ff.formFactor.value" columns="repeat(auto-fill, minmax(300px, 1fr))" />
  </div>
</template>

<style scoped>
.services-desktop { display: flex; flex-direction: column; gap: 18px; }
.top { align-items: flex-start; gap: 16px; }
.now {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 10px 14px;
  min-height: 44px;
  border-radius: var(--radius-l);
  background: var(--md-primary-container);
  color: var(--md-on-primary-container);
  flex: none;
}
.now.none { background: var(--md-surface-container); color: var(--md-on-surface-variant); }
.now-text { font-weight: 600; font-size: 14px; }
.exit { padding: 6px 12px; font-size: 12.5px; }
</style>
