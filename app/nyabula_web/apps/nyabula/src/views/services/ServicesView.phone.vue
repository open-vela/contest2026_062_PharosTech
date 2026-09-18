<script setup lang="ts">
/* Phone: "now showing" strip on top, then each group as a 2-column tile
 * grid. Tiles open the feature page; exit via the strip button. */
import { MdButton, NkListSection, UiIcon } from '@nyabula/ui';
import { inject } from 'vue';
import type { useFormFactor } from '../../composables/useFormFactor';
import { useServicesPage } from './services.logic';
import FeatureCard from './FeatureCard.vue';

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

    <NkListSection v-for="g in page.groups" :key="g.group" :title="g.group" :card="false">
      <div class="tiles">
        <FeatureCard
          v-for="f in g.items"
          :key="f.type"
          :def="f"
          :active="page.isActive(f)"
          :sub="page.subFor(f)"
          :ff="ff.formFactor.value"
          :payload="page.isActive(f) ? page.livePayload.value : null"
          @open="page.open(f)"
        />
      </div>
    </NkListSection>
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
.tiles { display: grid; grid-template-columns: 1fr; gap: 12px; align-items: start; }
</style>
