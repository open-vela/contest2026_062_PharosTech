<script setup lang="ts">
/* Settings hub: which device is being configured, its sections grouped
 * the same way as the device directory, then the browser-only settings.
 * One template for every form factor: the tile grid reflows from three
 * columns to one. */
import { computed } from 'vue';
import { useRoute } from 'vue-router';
import { NyabulaLogo, UiIcon } from '@nyabula/ui';
import DeviceSwitcher from '../../components/DeviceSwitcher.vue';
import { DEVICE_SECTION_GROUPS, DEVICE_SECTIONS } from '../../nav';
import { useSessionStore } from '../../stores/session';

const route = useRoute();
const session = useSessionStore();
const key = computed(() => route.params.key);
const groups = computed(() =>
  DEVICE_SECTION_GROUPS.map((g) => ({ ...g, items: DEVICE_SECTIONS.filter((s) => s.group === g.id) })),
);
const ROLE: Record<string, string> = { owner: '主人', family: '家人', guest: '访客' };
const status = computed(() => {
  if (session.connected) return { tone: 'ok', text: '在线' };
  if (session.state === 'connecting' || session.state === 'reconnecting' || session.state === 'authenticating') return { tone: 'warn', text: '连接中' };
  return { tone: 'err', text: '未连接' };
});
</script>

<template>
  <div class="settings-hub">
    <header class="hub-head">
      <h1 class="page-title">设置</h1>
      <p class="page-sub">设备设置由 Core 保存并应用到设备；客户端设置只保存在当前浏览器。</p>
    </header>

    <section class="ui-panel device-card" aria-label="当前设备">
      <NyabulaLogo class="avatar" :size="52" />
      <div class="who">
        <p class="name">{{ session.device?.name ?? '未选择设备' }}</p>
        <p class="meta">
          <span class="tag" :class="status.tone">{{ status.text }}</span>
          <span v-if="session.role" class="tag">{{ ROLE[session.role] ?? session.role }}</span>
          <span v-if="session.device?.coreVersion" class="tag mono">Core {{ session.device.coreVersion }}</span>
        </p>
      </div>
      <DeviceSwitcher class="switch" />
    </section>

    <section v-for="g in groups" :key="g.id" class="group" :aria-label="g.label">
      <h2 class="section-title">{{ g.label }}</h2>
      <div class="tiles">
        <RouterLink
          v-for="s in g.items"
          :key="s.id"
          class="list-tile hub-tile"
          :to="{ name: 'device', params: { key, section: s.id } }"
        >
          <span class="tile-icon"><UiIcon :name="s.icon" :size="22" /></span>
          <span class="tile-body">
            <span class="tile-title">{{ s.label }}</span>
            <span class="tile-sub">{{ s.desc }}</span>
          </span>
          <span class="tile-trail"><UiIcon name="chevron_right" :size="20" /></span>
        </RouterLink>
      </div>
    </section>

    <section class="group" aria-label="账号与客户端">
      <h2 class="section-title">账号与客户端</h2>
      <div class="tiles two">
        <RouterLink class="list-tile hub-tile" to="/account">
          <span class="tile-icon"><UiIcon name="person" :size="22" /></span>
          <span class="tile-body"><span class="tile-title">账号与云</span><span class="tile-sub">登录、设备认领、在线统计</span></span>
          <span class="tile-trail"><UiIcon name="chevron_right" :size="20" /></span>
        </RouterLink>
        <RouterLink class="list-tile hub-tile" to="/settings">
          <span class="tile-icon"><UiIcon name="tune" :size="22" /></span>
          <span class="tile-body"><span class="tile-title">客户端设置</span><span class="tile-sub">界面风格、深浅模式、色盘、布局与开发选项</span></span>
          <span class="tile-trail"><UiIcon name="chevron_right" :size="20" /></span>
        </RouterLink>
      </div>
    </section>
  </div>
</template>

<style scoped>
.settings-hub {
  max-width: 1040px;
  margin: 0 auto;
  padding: 24px 28px 48px;
  display: flex;
  flex-direction: column;
  gap: 22px;
}
.hub-head .page-sub { margin-bottom: 0; }
.device-card {
  display: flex;
  align-items: center;
  gap: 16px;
  padding: 18px 20px;
}
.avatar { flex: none; }
.who { flex: 1; min-width: 0; }
.name { margin: 0 0 6px; font: 600 18px var(--font-title); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.meta { margin: 0; display: flex; flex-wrap: wrap; gap: 6px; }
.switch { flex: none; }
.group .section-title { margin: 0 0 10px; }
.tiles { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 10px; }
.tiles.two { grid-template-columns: repeat(2, minmax(0, 1fr)); }
.hub-tile { min-height: 68px; }
/* Let the subtitle wrap instead of being cut off. */
.hub-tile .tile-sub { white-space: normal; }
@media (max-width: 900px) {
  .tiles { grid-template-columns: repeat(2, minmax(0, 1fr)); }
}
@media (max-width: 600px) {
  .settings-hub { padding: 12px 14px calc(28px + var(--shell-bottom) + var(--safe-b)); gap: 18px; }
  .tiles, .tiles.two { grid-template-columns: 1fr; gap: 8px; }
  .device-card { flex-wrap: wrap; padding: 16px; }
  .switch { width: 100%; }
}
</style>
