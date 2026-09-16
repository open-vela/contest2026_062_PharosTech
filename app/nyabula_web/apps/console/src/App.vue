<script setup lang="ts">
/* Console shell: topbar (nav / user / theme) + page switch. */
import { onMounted, ref } from 'vue';
import { MdButton } from '@nyabula/ui';
import { useAuthStore } from './stores/auth';
import { useNavStore } from './stores/nav';
import LoginView from './views/LoginView.vue';
import OverviewView from './views/OverviewView.vue';
import DevicesView from './views/DevicesView.vue';
import DeviceDetailView from './views/DeviceDetailView.vue';

const auth = useAuthStore();
const nav = useNavStore();
const theme = ref<'dark' | 'light'>('dark');

onMounted(() => void auth.init());

function toggleTheme() {
  theme.value = theme.value === 'dark' ? 'light' : 'dark';
  document.documentElement.dataset.theme = theme.value;
}

function logout() {
  auth.logout();
  nav.go('overview');
}
</script>

<template>
  <div class="shell">
    <header class="topbar">
      <span class="logo">Nyabula<span class="logo-dot">·</span>Console</span>
      <nav v-if="auth.user" class="nav">
        <button class="nav-btn" :class="{ active: nav.page === 'overview' }" @click="nav.go('overview')">总览</button>
        <button class="nav-btn" :class="{ active: nav.page !== 'overview' }" @click="nav.go('devices')">设备</button>
      </nav>
      <div class="topbar-right">
        <span v-if="auth.user" class="user-name">{{ auth.user.name || auth.user.email }}</span>
        <MdButton variant="icon" title="切换主题" @click="toggleTheme">◐</MdButton>
        <MdButton v-if="auth.user" variant="icon" title="登出" @click="logout">⏻</MdButton>
      </div>
    </header>
    <main class="main">
      <template v-if="auth.ready">
        <LoginView v-if="!auth.user" />
        <OverviewView v-else-if="nav.page === 'overview'" />
        <DeviceDetailView
          v-else-if="nav.page === 'device' && nav.deviceId"
          :key="nav.deviceId"
          :device-id="nav.deviceId"
        />
        <DevicesView v-else />
      </template>
    </main>
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
  gap: 24px;
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
.nav {
  display: flex;
  gap: 6px;
  flex: 1;
}
.nav-btn {
  background: transparent;
  border: none;
  color: var(--md-on-surface-variant);
  font: 600 14px var(--font-body);
  padding: 8px 14px;
  border-radius: var(--radius-full);
  cursor: pointer;
}
.nav-btn:hover {
  color: var(--md-on-surface);
  background: var(--md-surface-container-high);
}
.nav-btn.active {
  background: var(--md-secondary-container);
  color: var(--md-on-secondary-container);
}
.topbar-right {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-left: auto;
}
.user-name {
  font: 600 13px var(--font-body);
  color: var(--md-on-surface-variant);
}
.main {
  flex: 1;
  min-height: 0;
  overflow-y: auto;
}
</style>
