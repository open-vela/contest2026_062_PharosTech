<script setup lang="ts">
import { computed, ref } from 'vue';
import { MdButton } from '@nyabula/ui';
import { useLinkStore } from './stores/link';
import StatusPill from './components/StatusPill.vue';
import ConnectView from './views/ConnectView.vue';
import HomeView from './views/HomeView.vue';
import PluginsView from './views/PluginsView.vue';
import PermissionsView from './views/PermissionsView.vue';
import SettingsView from './views/SettingsView.vue';

type Tab = 'home' | 'plugins' | 'permissions' | 'settings';

const link = useLinkStore();
const theme = ref<'dark' | 'light'>('dark');
/* The app shell (with the eye area) also hosts the pairing overlay, so it
 * must render while the link is authenticating / awaiting the pair code. */
const inApp = computed(() =>
  ['connected', 'authenticating', 'pairing-required'].includes(link.state),
);
const tab = ref<Tab>('home');

const TABS: { key: Tab; label: string; glyph: string }[] = [
  { key: 'home', label: '概览', glyph: '◉' },
  { key: 'plugins', label: '插件', glyph: '▦' },
  { key: 'permissions', label: '权限', glyph: '✓' },
  { key: 'settings', label: '设置', glyph: '⚙' },
];

function toggleTheme() {
  theme.value = theme.value === 'dark' ? 'light' : 'dark';
  document.documentElement.dataset.theme = theme.value;
}
</script>

<template>
  <div class="shell">
    <header class="topbar">
      <span class="logo">Nyabula<span class="logo-dot">·</span>Panel</span>
      <div class="topbar-right">
        <StatusPill />
        <MdButton variant="icon" title="切换主题" @click="toggleTheme">◐</MdButton>
        <MdButton v-if="inApp" variant="icon" title="断开" @click="link.disconnect()">⏻</MdButton>
      </div>
    </header>
    <div class="body">
      <!-- Desktop navigation rail (hidden on mobile via CSS) -->
      <nav v-if="inApp" class="rail">
        <button
          v-for="t in TABS"
          :key="t.key"
          class="rail-item"
          :class="{ active: tab === t.key }"
          @click="tab = t.key"
        >
          <span class="rail-glyph">{{ t.glyph }}</span>
          <span class="rail-label">{{ t.label }}</span>
        </button>
      </nav>
      <main class="main">
        <template v-if="inApp">
          <HomeView v-if="tab === 'home'" />
          <PluginsView v-else-if="tab === 'plugins'" />
          <PermissionsView v-else-if="tab === 'permissions'" />
          <SettingsView v-else />
        </template>
        <ConnectView v-else />
      </main>
    </div>
    <!-- Mobile bottom navigation bar (hidden on desktop via CSS) -->
    <nav v-if="inApp" class="bottombar">
      <button
        v-for="t in TABS"
        :key="t.key"
        class="bottom-item"
        :class="{ active: tab === t.key }"
        @click="tab = t.key"
      >
        <span class="bottom-glyph">{{ t.glyph }}</span>
        <span class="bottom-label">{{ t.label }}</span>
      </button>
    </nav>
  </div>
</template>

<style scoped>
.shell {
  height: 100%;
  display: flex;
  flex-direction: column;
}
.topbar {
  height: 58px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 20px;
  background: var(--md-surface-container);
  box-shadow: var(--md-elev-1);
  flex: none;
}
.logo {
  font-family: var(--font-title);
  font-size: 19px;
  color: var(--md-on-surface);
  letter-spacing: 1px;
}
.logo-dot {
  color: var(--md-primary);
  margin: 0 2px;
}
.topbar-right {
  display: flex;
  align-items: center;
  gap: 10px;
}
.body {
  flex: 1;
  min-height: 0;
  display: flex;
}
.rail {
  width: 84px;
  flex: none;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 6px;
  padding-top: 16px;
  background: var(--md-surface-container);
}
.rail-item {
  width: 68px;
  background: transparent;
  border: none;
  color: var(--md-on-surface-variant);
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 4px;
  padding: 10px 0;
  border-radius: var(--radius-m);
  cursor: pointer;
}
.rail-item:hover { background: var(--md-surface-container-high); }
.rail-item.active {
  color: var(--md-on-secondary-container);
  background: var(--md-secondary-container);
}
.rail-glyph { font-size: 18px; }
.rail-label { font: 600 11px var(--font-body); }
.main {
  flex: 1;
  min-width: 0;
  min-height: 0;
  overflow-y: auto;
}
.bottombar {
  display: none;
  flex: none;
  background: var(--md-surface-container);
  box-shadow: 0 -1px 3px rgba(0, 0, 0, 0.3);
}
.bottom-item {
  flex: 1;
  background: transparent;
  border: none;
  color: var(--md-on-surface-variant);
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 2px;
  padding: 8px 0 10px;
  cursor: pointer;
}
.bottom-item.active { color: var(--md-primary); }
.bottom-glyph { font-size: 18px; }
.bottom-label { font: 600 11px var(--font-body); }

@media (max-width: 720px) {
  .rail { display: none; }
  .bottombar { display: flex; }
}
</style>
