<script setup lang="ts">
import TrafficPage from '../../components/TrafficPage.vue';
import EventsPage from '../../components/EventsPage.vue';
/* Classic skin shell (the original console): topbar (nav / user / theme)
 * + page switch. Appearance settings live on the account page. */
import { MdButton, NyabulaLogo, UiIcon, eventOrigin } from '@nyabula/ui';
import { useAuthStore } from '../../stores/auth';
import { useNavStore } from '../../stores/nav';
import { useAppearance } from '../../logic/useAppearance';
import ClassicLogin from './ClassicLogin.vue';
import ClassicOverview from './ClassicOverview.vue';
import ClassicDevices from './ClassicDevices.vue';
import ClassicDeviceDetail from './ClassicDeviceDetail.vue';
import ClassicAccount from './ClassicAccount.vue';

const auth = useAuthStore();
const nav = useNavStore();
const ap = useAppearance();

async function logout() {
  if (await auth.logout()) nav.go('overview');
}
</script>

<template>
  <div class="shell">
    <header class="topbar">
      <span class="logo"><NyabulaLogo :size="28" /><span>Nyabula<span class="logo-dot">·</span>Cloud</span></span>
      <nav v-if="auth.user" class="nav">
        <button class="nav-btn" :class="{ active: nav.page === 'overview' }" @click="nav.go('overview')">总览</button>
        <button class="nav-btn" :class="{ active: nav.page === 'devices' || nav.page === 'device' }" @click="nav.go('devices')">设备</button>
        <button class="nav-btn" :class="{ active: nav.page === 'traffic' }" @click="nav.go('traffic')">流量</button><button class="nav-btn" :class="{ active: nav.page === 'events' }" @click="nav.go('events')">事件</button>
        <button class="nav-btn" :class="{ active: nav.page === 'account' }" @click="nav.go('account')">账号</button>
      </nav>
      <div class="topbar-right">
        <button v-if="auth.user" class="user-name" @click="nav.go('account')">{{ auth.user.name || auth.user.email }}</button>
        <MdButton variant="icon" :title="ap.theme.mode === 'dark' ? '切换浅色' : '切换深色'" @click="ap.toggleMode(eventOrigin($event))">
          <UiIcon :name="ap.theme.mode === 'dark' ? 'dark_mode' : 'light_mode'" :size="20" />
        </MdButton>
        <MdButton v-if="auth.user" variant="icon" title="登出" @click="logout"><UiIcon name="logout" :size="20" /></MdButton>
      </div>
    </header>
    <main class="main">
      <p v-if="auth.lastError && auth.user && nav.page !== 'account'" role="alert">{{ auth.lastError }}</p>
      <ClassicLogin v-if="!auth.user" />
      <Transition v-else name="page" mode="out-in">
        <ClassicOverview v-if="nav.page === 'overview'" key="o" />
        <ClassicDeviceDetail v-else-if="nav.page === 'device' && nav.deviceId" :key="nav.deviceId" :device-id="nav.deviceId" />
        <TrafficPage v-else-if="nav.page === 'traffic'" key="t" /><EventsPage v-else-if="nav.page === 'events'" key="e" />
        <ClassicAccount v-else-if="nav.page === 'account'" key="a" />
        <ClassicDevices v-else key="d" />
      </Transition>
    </main>
  </div>
</template>

<style scoped>
.shell { height: 100dvh; display: flex; flex-direction: column; background: var(--md-surface); }
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
.logo { display: inline-flex; align-items: center; gap: 10px; font-family: var(--font-title); font-size: 19px; color: var(--md-on-surface); letter-spacing: 1px; white-space: nowrap; }
.logo-dot { color: var(--md-primary); margin: 0 2px; }
.nav { display: flex; gap: 6px; flex: 1; }
.nav-btn {
  background: transparent;
  border: none;
  color: var(--md-on-surface-variant);
  font: 600 14px var(--font-body);
  padding: 8px 14px;
  border-radius: var(--radius-full);
  cursor: pointer;
}
.nav-btn:hover { color: var(--md-on-surface); background: var(--md-surface-container-high); }
.nav-btn.active { background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.topbar-right { display: flex; align-items: center; gap: 6px; margin-left: auto; }
.user-name { border: none; background: none; cursor: pointer; font: 600 13px var(--font-body); color: var(--md-on-surface-variant); }
.user-name:hover { color: var(--md-on-surface); }
.main { flex: 1; min-height: 0; overflow-y: auto; }
@media (max-width: 720px) {
  .topbar { gap: 10px; padding: 0 10px; }
  .logo > span, .user-name { display: none; }
  .nav-btn { padding: 8px 10px; }
}
</style>
