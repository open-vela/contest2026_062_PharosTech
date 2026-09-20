<script setup lang="ts">
import { useRoute } from 'vue-router';
import { UiIcon } from '@nyabula/ui';
import DeviceSwitcher from '../../components/DeviceSwitcher.vue';
import { DEVICE_SECTIONS } from '../../nav';

const route = useRoute();
</script>

<template>
  <div class="settings-hub">
    <section aria-labelledby="device-settings-title">
      <h2 id="device-settings-title">设备设置</h2>
      <p class="muted">修改当前设备，由 Core 保存和应用。</p>
      <DeviceSwitcher />
      <div class="settings-links">
        <RouterLink v-for="section in DEVICE_SECTIONS" :key="section.id" class="list-tile"
          :to="{ name: 'device', params: { key: route.params.key, section: section.id } }">
          <UiIcon :name="section.icon" :size="22" />
          <span class="tile-body">{{ section.label }}</span>
          <UiIcon name="chevron_right" :size="20" />
        </RouterLink>
      </div>
    </section>
    <section aria-labelledby="personal-settings-title">
      <h2 id="personal-settings-title">账号与客户端</h2>
      <RouterLink class="list-tile" to="/account">
        <UiIcon name="person" :size="22" />
        <span class="tile-body"><span class="tile-title">账号与云</span><span class="tile-sub">登录、设备认领、统计</span></span>
        <UiIcon name="chevron_right" :size="20" />
      </RouterLink>
      <RouterLink class="list-tile" to="/settings">
        <UiIcon name="tune" :size="22" />
        <span class="tile-body"><span class="tile-title">客户端设置</span><span class="tile-sub">仅当前浏览器的主题、布局和开发选项</span></span>
        <UiIcon name="chevron_right" :size="20" />
      </RouterLink>
    </section>
  </div>
</template>

<style scoped>
.settings-hub { max-width: 1000px; margin: 0 auto; padding: 20px; display: grid; gap: 28px; }
h2 { font: 600 20px var(--font-title); margin: 0 0 8px; }
.settings-links { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 4px; margin-top: 12px; }
.list-tile { min-height: 52px; }
@media (max-width: 600px) { .settings-hub { padding: 16px; } }
</style>
