<script setup lang="ts">
import { computed } from 'vue';
import { NyabulaLogo, ThemeSwitcher, UiIcon } from '@nyabula/ui';
import { useAuthStore } from '../../stores/auth';
import { useNavStore, type Page } from '../../stores/nav';
import ClassicLogin from '../classic/ClassicLogin.vue';
import ClassicOverview from '../classic/ClassicOverview.vue';
import ClassicDevices from '../classic/ClassicDevices.vue';
import ClassicDeviceDetail from '../classic/ClassicDeviceDetail.vue';
import ClassicAccount from '../classic/ClassicAccount.vue';
import TrafficPage from '../../components/TrafficPage.vue';
import EventsPage from '../../components/EventsPage.vue';

const auth = useAuthStore();
const nav = useNavStore();
const sections: { id: Page; title: string; icon: string }[] = [
  { id: 'overview', title: '总览', icon: 'dashboard' },
  { id: 'devices', title: '设备', icon: 'devices' },
  { id: 'traffic', title: '流量', icon: 'trending_up' },
  { id: 'events', title: '事件', icon: 'history' },
  { id: 'account', title: '账号', icon: 'person' },
];
const section = computed(() => nav.page === 'device' ? 'devices' : nav.page);
const title = computed(() => sections.find(s => s.id === section.value)?.title ?? 'Cloud');
</script>

<template>
  <div class="day-cloud" :class="{ guest: !auth.user }">
    <aside v-if="auth.user" class="sidebar">
      <div class="brand"><NyabulaLogo :size="44" /><span>Nyabula<small>CLOUD · 日光</small></span></div>
      <nav aria-label="Cloud 导航"><button v-for="s in sections" :key="s.id" :class="{ on: section === s.id }" :aria-current="section === s.id ? 'page' : undefined" @click="nav.go(s.id)"><UiIcon :name="s.icon" :size="21" /><span>{{ s.title }}</span></button></nav>
      <button class="profile" @click="nav.go('account')"><span class="avatar">{{ (auth.user.name || auth.user.email).slice(0,1) }}</span><span>{{ auth.user.name || auth.user.email }}</span></button>
    </aside>
    <div class="body">
      <header><div><small>Nyabula Cloud</small><h1>{{ auth.user ? title : '欢迎回家' }}</h1></div><ThemeSwitcher /></header>
      <main>
        <ClassicLogin v-if="!auth.user" />
        <Transition v-else name="page" mode="out-in">
          <ClassicOverview v-if="nav.page === 'overview'" key="overview" />
          <ClassicDevices v-else-if="nav.page === 'devices'" key="devices" />
          <ClassicDeviceDetail v-else-if="nav.page === 'device' && nav.deviceId" :key="nav.deviceId" :device-id="nav.deviceId" />
          <TrafficPage v-else-if="nav.page === 'traffic'" key="traffic" />
          <EventsPage v-else-if="nav.page === 'events'" key="events" />
          <ClassicAccount v-else key="account" />
        </Transition>
      </main>
    </div>
  </div>
</template>
<style scoped>
.day-cloud { height: 100dvh; display: grid; grid-template-columns: 230px minmax(0,1fr); background: var(--md-surface); overflow: hidden; }
.day-cloud.guest { grid-template-columns: minmax(0,1fr); }
.guest header { padding-top: 74px; }
.sidebar { background: var(--md-surface-container); border-right: 1px solid var(--md-outline-variant); padding: 26px 18px; display: flex; flex-direction: column; gap: 36px; }
.brand { display: flex; gap: 12px; align-items: center; font: 22px var(--font-title); }.brand small { display: block; margin-top: 6px; font: 11px var(--font-body); letter-spacing: .12em; color: var(--md-on-surface-variant); }
nav { display: flex; flex-direction: column; gap: 8px; }nav button { display: flex; align-items: center; gap: 12px; padding: 14px 18px; border: 0; border-radius: 17px; background: transparent; color: var(--md-on-surface-variant); font: 600 14px var(--font-body); cursor: pointer; }nav button.on { color: var(--md-on-primary-container); background: var(--md-primary-container); }
.profile { margin-top: auto; display: flex; align-items: center; gap: 10px; border: 0; border-radius: 18px; padding: 12px; color: var(--md-on-surface); background: var(--md-surface-container-high); text-align: left; overflow-wrap: anywhere; cursor: pointer; }
.avatar { width: 36px; height: 36px; flex: none; display: grid; place-items: center; border-radius: 12px; background: var(--dl-apricot); color: #352315; }
.body { min-width: 0; min-height: 0; display: flex; flex-direction: column; }header { display: flex; align-items: center; justify-content: space-between; padding: 22px 30px; gap: 16px; }header h1 { margin: 4px 0 0; font: 26px var(--font-title); }header small { color: var(--md-on-surface-variant); }main { flex: 1; min-height: 0; overflow: auto; }
main :deep(.page) { max-width: 1200px; margin: auto; padding: 12px 30px 40px; }main :deep(.grid) { gap: 18px; }main :deep(.big) { font-size: 38px; }main :deep(.md-card:nth-child(2)) { background: color-mix(in srgb,var(--dl-apricot) 10%,var(--md-surface-container)); }
@media(max-width:760px) { .day-cloud { grid-template-columns: minmax(0,1fr); }.sidebar { position: fixed; bottom: 0; left: 0; right: 0; z-index: 50; padding: 6px 8px calc(6px + env(safe-area-inset-bottom)); border-top: 1px solid var(--md-outline-variant); }.brand,.profile { display: none; }nav { flex-direction: row; gap: 2px; }nav button { flex: 1; flex-direction: column; padding: 10px 3px; gap: 4px; font-size: 12px; }header { padding: 14px 16px; }header h1 { font-size: 22px; }main { padding-bottom: 84px; }main :deep(.page) { padding: 12px 16px 26px; } }
</style>
