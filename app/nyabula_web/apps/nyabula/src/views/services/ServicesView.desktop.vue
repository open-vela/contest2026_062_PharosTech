<script setup lang="ts">
/* Desktop: "now showing" bar, then each group as NkListSection with a
 * 4-column tile grid. Tiles open the feature page. */
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

    <NkListSection v-for="g in page.groups" :key="g.group" :title="g.group" :card="false" v-reveal>
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
.tiles { display: grid; grid-template-columns: repeat(auto-fill, minmax(300px, 1fr)); gap: 12px; align-items: start; }
</style>
