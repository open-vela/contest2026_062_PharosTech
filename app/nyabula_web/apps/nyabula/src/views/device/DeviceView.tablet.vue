<script setup lang="ts">
/* Tablet: landscape = directory + content (like desktop); portrait =
 * horizontally scrollable segmented tabs above the content. */
import { computed, inject } from 'vue';
import { SegmentedTabs } from '@nyabula/ui';
import DeviceSectionNav from './DeviceSectionNav.vue';
import DeviceSectionHead from './DeviceSectionHead.vue';
import { useDevicePage } from './device.logic';
import type { useFormFactor } from '../../composables/useFormFactor';

const props = defineProps<{ section?: string }>();
const page = useDevicePage(props);
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const portrait = computed(() => ff.orientation.value === 'portrait');
const tabItems = computed(() => page.sections.value.map((s) => ({ id: s.id, label: s.label, icon: s.icon })));
const tab = computed({
  get: () => page.current.value.id,
  set: (id: string) => page.go(id),
});
</script>

<template>
  <div class="device-tablet" :class="{ portrait }">
    <DeviceSectionNav v-if="!portrait" :page="page" compact />
    <div v-else class="tabs-scroll">
      <SegmentedTabs v-model="tab" :items="tabItems" />
    </div>
    <section class="content">
      <DeviceSectionHead :section="page.current.value" :device="page.session.device?.name" />
      <Transition name="page" mode="out-in">
        <component :is="page.current.value.component" :key="page.current.value.id" :section="page.current.value.id" />
      </Transition>
    </section>
  </div>
</template>

<style scoped>
.device-tablet {
  display: grid;
  grid-template-columns: 200px minmax(0, 1fr);
  gap: 18px;
  padding: 18px 20px 40px;
}
.device-tablet.portrait { grid-template-columns: 1fr; }
.tabs-scroll { overflow-x: auto; scrollbar-width: none; padding-bottom: 2px; }
.tabs-scroll::-webkit-scrollbar { display: none; }
.content { min-width: 0; }
</style>
