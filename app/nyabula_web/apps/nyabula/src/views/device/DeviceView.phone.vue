<script setup lang="ts">
/* Phone: no section -> list-tile directory; section -> content only (the
 * shell app bar owns back navigation; an inline crumb returns to the list). */
import { UiIcon } from '@nyabula/ui';
import { useDevicePage } from './device.logic';

const props = defineProps<{ section?: string }>();
const page = useDevicePage(props);
</script>

<template>
  <div class="device-phone">
    <template v-if="!page.requested.value">
      <h1 class="page-title">设备</h1>
      <p class="page-sub">{{ page.session.device?.name ?? '未连接' }}</p>
      <div class="stack">
        <button v-for="s in page.sections.value" :key="s.id" class="list-tile" @click="page.go(s.id)">
          <span class="tile-icon"><UiIcon :name="s.icon" :size="22" /></span>
          <span class="tile-body">
            <span class="tile-title">{{ s.label }}</span>
            <span v-if="s.extension" class="tile-sub">扩展段</span>
          </span>
          <span class="tile-trail"><UiIcon name="chevron_right" :size="20" /></span>
        </button>
      </div>
    </template>
    <template v-else>
      <button class="crumb" @click="page.go(null)"><UiIcon name="chevron_left" :size="18" /> 设备目录</button>
      <h1 class="page-title">{{ page.current.value.label }}</h1>
      <Transition name="page" mode="out-in">
        <component :is="page.current.value.component" :key="page.current.value.id" :section="page.current.value.id" />
      </Transition>
    </template>
  </div>
</template>

<style scoped>
.device-phone { padding: 8px 14px calc(28px + var(--shell-bottom) + var(--safe-b)); }
.list-tile { width: 100%; text-align: left; border: none; font: inherit; }
.crumb {
  display: inline-flex;
  align-items: center;
  gap: 2px;
  border: none;
  background: transparent;
  color: var(--md-primary);
  font: 600 13px var(--font-body);
  padding: 4px 0 8px;
  cursor: pointer;
}
</style>
