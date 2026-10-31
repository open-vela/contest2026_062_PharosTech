<script setup lang="ts">
/* Desktop: sticky grouped directory on the left, content on the right. */
import { useDevicePage } from './device.logic';
import DeviceSectionNav from './DeviceSectionNav.vue';
import DeviceSectionHead from './DeviceSectionHead.vue';

const props = defineProps<{ section?: string }>();
const page = useDevicePage(props);
</script>

<template>
  <div class="device-desktop">
    <DeviceSectionNav :page="page" />
    <section class="content">
      <DeviceSectionHead :section="page.current.value" :device="page.session.device?.name" />
      <Transition name="page" mode="out-in">
        <component :is="page.current.value.component" :key="page.current.value.id" :section="page.current.value.id" />
      </Transition>
    </section>
  </div>
</template>

<style scoped>
.device-desktop {
  display: grid;
  grid-template-columns: 232px minmax(0, 1fr);
  gap: 32px;
  padding: 24px 28px 48px;
  max-width: 1200px;
  margin: 0 auto;
  width: 100%;
}
.content { min-width: 0; max-width: 880px; }
</style>
