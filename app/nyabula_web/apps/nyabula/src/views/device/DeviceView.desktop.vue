<script setup lang="ts">
/* Desktop: sticky section directory on the left, content on the right. */
import { UiIcon } from '@nyabula/ui';
import { useDevicePage } from './device.logic';

const props = defineProps<{ section?: string }>();
const page = useDevicePage(props);
</script>

<template>
  <div class="device-desktop">
    <nav class="dir">
      <button
        v-for="s in page.sections.value"
        :key="s.id"
        class="dir-item"
        :class="{ on: page.current.value.id === s.id, ext: s.extension }"
        @click="page.go(s.id)"
      >
        <UiIcon :name="s.icon" :size="20" />
        <span>{{ s.label }}</span>
      </button>
    </nav>
    <section class="content">
      <h1 class="page-title">{{ page.current.value.label }}</h1>
      <p class="page-sub">
        {{ page.session.device?.name ?? '设备' }}
        <span v-if="page.current.value.extension" class="tag info" style="margin-left: 8px">扩展</span>
      </p>
      <Transition name="page" mode="out-in">
        <component :is="page.current.value.component" :key="page.current.value.id" :section="page.current.value.id" />
      </Transition>
    </section>
  </div>
</template>

<style scoped>
.device-desktop {
  display: grid;
  grid-template-columns: 220px minmax(0, 1fr);
  gap: 24px;
  padding: 20px 24px 40px;
  max-width: 1200px;
  margin: 0 auto;
  width: 100%;
}
.dir {
  position: sticky;
  top: 16px;
  align-self: start;
  display: flex;
  flex-direction: column;
  gap: 2px;
}
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
  transition: background var(--dur-fast), color var(--dur-fast);
}
.dir-item:hover { background: var(--md-surface-container); }
.dir-item.on { background: var(--md-secondary-container); color: var(--md-on-secondary-container); font-weight: 600; }
.dir-item.ext { border: 1px dashed var(--md-outline-variant); }
.content { min-width: 0; }
</style>
