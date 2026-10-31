<script setup lang="ts">
import TrafficPage from '../../components/TrafficPage.vue';
import EventsPage from '../../components/EventsPage.vue';
/* Nebula skin shell: sign-in morphs into a console with a floating glass
 * sidebar (sliding light marks the page) and a top bar. On phones the
 * sidebar becomes a floating bottom tab bar. Device detail opens as a
 * drawer over the device list (nav.page === 'device'). */
import { computed } from 'vue';
import { ThemeSwitcher, UiIcon } from '@nyabula/ui';
import './nebula.css';
import NebulaAurora from './NebulaAurora.vue';
import NebulaLogin from './NebulaLogin.vue';
import NebulaOverview from './NebulaOverview.vue';
import NebulaDevices from './NebulaDevices.vue';
import NebulaAccount from './NebulaAccount.vue';
import LiveEyes from '../../components/LiveEyes.vue';
import { useAuthStore } from '../../stores/auth';
import { useNavStore, type Page } from '../../stores/nav';
import { useDevicesStore } from '../../stores/devices';

const auth = useAuthStore();
const nav = useNavStore();
const devices = useDevicesStore();

const NAV: { id: Page; label: string; icon: string }[] = [
  { id: 'overview', label: '总览', icon: 'dashboard' },
  { id: 'devices', label: '设备', icon: 'devices' },
  { id: 'traffic', label: '流量', icon: 'trending_up' },
  { id: 'events', label: '事件', icon: 'history' },
  { id: 'account', label: '账号', icon: 'person' },
];
/* The detail drawer lives on the devices page. */
const section = computed<Page>(() => (nav.page === 'device' ? 'devices' : nav.page));
const idx = computed(() => Math.max(0, NAV.findIndex((n) => n.id === section.value)));
const online = computed(() => devices.devices.filter((d) => d.online).length);
const initial = computed(() => (auth.user?.name || auth.user?.email || '?').slice(0, 1).toUpperCase());

function go(p: Page) {
  nav.go(p);
}
</script>

<template>
  <div class="nbc">
    <NebulaAurora />
    <Transition name="enter" mode="out-in">
      <NebulaLogin v-if="!auth.user" key="login" />
      <div v-else key="console" class="console">
        <aside class="side nb-glass">
          <div class="brand">
            <div class="brand-eyes"><LiveEyes mode="happy" :follow="false" /></div>
            <div><b class="nb-title">Nyabula</b><small>Cloud</small></div>
          </div>
          <nav class="nav" :style="{ '--i': idx, '--n': NAV.length }">
            <span class="nav-light" />
            <button v-for="n in NAV" :key="n.id" :class="{ on: section === n.id }" :aria-current="section === n.id ? 'page' : undefined" @click="go(n.id)">
              <UiIcon :name="n.icon" :size="20" />
              <span>{{ n.label }}</span>
            </button>
          </nav>
          <div class="side-foot">
            <span class="nb-dot" :class="{ on: online > 0 }" />
            <span>{{ online }} / {{ devices.devices.length }} 台在线</span>
          </div>
        </aside>

        <div class="body">
          <header class="bar">
            <span class="crumb">Nyabula Cloud <UiIcon name="chevron_right" :size="16" /> <b>{{ NAV[idx]?.label }}</b></span>
            <ThemeSwitcher class="theme" />
            <button class="me nb-glass" title="账号与外观" @click="go('account')">
              <span class="av">{{ initial }}</span>
              <span class="me-t"><b>{{ auth.user.name || auth.user.email }}</b><small>{{ auth.user.email }}</small></span>
            </button>
          </header>
          <main class="main">
            <Transition name="nb-page" mode="out-in">
              <NebulaOverview v-if="section === 'overview'" key="o" />
              <NebulaDevices v-else-if="section === 'devices'" key="d" />
              <TrafficPage v-else-if="nav.page === 'traffic'" key="t" /><EventsPage v-else-if="nav.page === 'events'" key="e" />
              <NebulaAccount v-else key="a" />
            </Transition>
          </main>
        </div>
      </div>
    </Transition>
  </div>
</template>

<style scoped>
.enter-enter-active { transition: opacity 0.8s var(--ease), transform 0.9s var(--ease), filter 0.8s; }
.enter-leave-active { transition: opacity 0.45s ease, transform 0.5s var(--ease), filter 0.45s; }
.enter-enter-from { opacity: 0; transform: scale(1.03); filter: blur(12px); }
.enter-leave-to { opacity: 0; transform: scale(0.94); filter: blur(10px); }
.console { display: grid; grid-template-columns: 240px minmax(0, 1fr); min-height: 100dvh; }
.side { position: sticky; top: 14px; margin: 14px 0 14px 14px; height: calc(100dvh - 28px); display: flex; flex-direction: column; gap: 18px; padding: 18px 14px; border-radius: 28px; z-index: 30; }
.brand { display: flex; align-items: center; gap: 10px; padding: 0 6px; }
.brand-eyes { width: 64px; height: 36px; border-radius: 12px; background: #000; overflow: hidden; border: 1px solid var(--line); flex: none; }
.brand b { font-size: 19px; display: block; line-height: 1.1; }
.brand small { color: var(--acc); font-size: 11px; letter-spacing: 0.24em; text-transform: uppercase; }
.nav { position: relative; display: flex; flex-direction: column; gap: 4px; }
.nav-light {
  position: absolute;
  left: 0;
  right: 0;
  top: 0;
  height: 46px;
  border-radius: 16px;
  background: linear-gradient(90deg, rgba(var(--acc-rgb), 0.2), rgba(var(--acc-rgb), 0.04));
  border: 1px solid rgba(var(--acc-rgb), 0.24);
  box-shadow: inset 0 1px 0 var(--hi);
  transform: translateY(calc(var(--i) * 50px));
  transition: transform 0.55s var(--spring);
}
.nav button { position: relative; height: 46px; display: flex; align-items: center; gap: 12px; padding: 0 14px; border: none; background: transparent; color: var(--sub); border-radius: 16px; cursor: pointer; font: 600 14px var(--font-body); transition: color 0.2s, transform 0.3s var(--spring); }
.nav button:hover { color: var(--fg); }
.nav button:active { transform: scale(0.97); }
.nav button.on { color: var(--fg); }
.nav button.on :deep(.ui-icon) { color: var(--acc); }
.side-foot { margin-top: auto; display: flex; align-items: center; gap: 8px; font-size: 12px; color: var(--sub); padding: 10px 12px; border-radius: 14px; background: var(--wash); }
.body { min-width: 0; }
.bar { position: sticky; top: 0; z-index: 20; display: flex; align-items: center; gap: 10px; padding: 18px clamp(16px, 2.6vw, 34px); }
.crumb { flex: 1; display: flex; align-items: center; gap: 4px; color: var(--sub); font-size: 13px; min-width: 0; white-space: nowrap; }
.crumb b { color: var(--fg); font-weight: 600; }
.me { display: flex; align-items: center; gap: 10px; padding: 5px 14px 5px 5px; border-radius: 999px; color: var(--fg); cursor: pointer; transition: transform 0.35s var(--spring); }
.me:active { transform: scale(0.96); }
.av { width: 32px; height: 32px; border-radius: 50%; display: grid; place-items: center; background: linear-gradient(135deg, var(--acc), var(--acc2)); color: var(--md-on-primary); font-family: var(--font-title); flex: none; }
.me-t { display: flex; flex-direction: column; align-items: flex-start; line-height: 1.2; max-width: 180px; }
.me-t b, .me-t small { max-width: 100%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.me-t small { color: var(--sub); font-size: 11px; }
.main { padding: 0 clamp(16px, 2.6vw, 34px) 60px; max-width: 1360px; }
@media (max-width: 860px) {
  .console { grid-template-columns: minmax(0, 1fr); }
  .side { position: fixed; top: auto; left: 10px; right: 10px; bottom: calc(10px + env(safe-area-inset-bottom)); margin: 0; height: auto; padding: 6px; border-radius: 24px; background: var(--panel-strong); }
  .brand, .side-foot { display: none; }
  .nav { flex-direction: row; }
  .nav-light { width: calc(100% / var(--n)); height: 100%; transform: translateX(calc(var(--i) * 100%)); }
  .nav button { flex: 1; flex-direction: column; height: 54px; gap: 3px; padding: 0; font-size: 11px; justify-content: center; }
  .main { padding-bottom: 110px; }
  .me-t { display: none; }
  .me { padding: 5px; }
  .theme { display: none; }
}
</style>
