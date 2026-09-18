<script setup lang="ts">
/* Tablet: landscape = directory + content (like desktop); portrait =
 * horizontally scrollable segmented tabs above the content. */
import { computed, inject } from 'vue';
import { SegmentedTabs, UiIcon } from '@nyabula/ui';
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
    <nav v-if="!portrait" class="dir">
      <button
        v-for="s in page.sections.value"
        :key="s.id"
        class="dir-item"
        :class="{ on: page.current.value.id === s.id }"
        @click="page.go(s.id)"
      >
        <UiIcon :name="s.icon" :size="20" />
        <span>{{ s.label }}</span>
      </button>
    </nav>
    <div v-else class="tabs-scroll">
      <SegmentedTabs v-model="tab" :items="tabItems" />
    </div>
    <section class="content">
      <h1 class="page-title">{{ page.current.value.label }}</h1>
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
.dir { position: sticky; top: 12px; align-self: start; display: flex; flex-direction: column; gap: 2px; }
.dir-item {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 10px 14px;
  border: none;
  border-radius: var(--radius-full);
  background: transparent;
  color: var(--md-on-surface-variant);
  font: 500 14px var(--font-body);
  text-align: left;
  cursor: pointer;
}
.dir-item.on { background: var(--md-secondary-container); color: var(--md-on-secondary-container); font-weight: 600; }
.tabs-scroll { overflow-x: auto; scrollbar-width: none; padding-bottom: 2px; }
.tabs-scroll::-webkit-scrollbar { display: none; }
.content { min-width: 0; }
</style>
