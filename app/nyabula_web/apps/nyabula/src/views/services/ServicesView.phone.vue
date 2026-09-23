<script setup lang="ts">
/* Phone: "now showing" strip on top, then the two stages (已实现 / 规划中),
 * each group as a single-column card list. Exit via the strip button. */
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
  <div class="page narrow services-phone">
    <h1 class="page-title">功能</h1>
    <div class="now" :class="{ none: !activeDef }">
      <span class="now-icon"><UiIcon :name="activeDef?.icon ?? 'face'" :size="20" /></span>
      <div class="now-body">
        <div class="now-title">{{ activeDef ? activeDef.label : session.connected ? '无（表情模式）' : '设备离线' }}</div>
        <div class="now-sub">{{ activeDef ? '正在显示' : '点击下方磁贴进入功能' }}</div>
      </div>
      <MdButton v-if="activeDef" variant="tonal" class="exit" :disabled="!session.canControl" @click="page.exit()">退出</MdButton>
    </div>

    <FeatureSections :page="page" :ff="ff.formFactor.value" columns="minmax(0, 1fr)" />
  </div>
</template>

<style scoped>
.services-phone { display: flex; flex-direction: column; gap: 14px; padding-bottom: 24px; }
.now {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px 14px;
  min-height: 56px;
  border-radius: var(--radius-l);
  background: var(--md-primary-container);
  color: var(--md-on-primary-container);
}
.now.none { background: var(--md-surface-container); color: var(--md-on-surface); }
.now-icon { width: 40px; height: 40px; border-radius: 12px; display: grid; place-items: center; background: var(--md-surface-container-highest); flex: none; }
.now-body { flex: 1; min-width: 0; }
.now-title { font-weight: 600; font-size: 14.5px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.now-sub { font-size: 12px; opacity: 0.75; }
.exit { min-height: 44px; padding: 8px 14px; font-size: 13px; flex: none; }
</style>
