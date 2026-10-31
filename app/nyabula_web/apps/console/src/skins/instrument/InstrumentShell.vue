<script setup lang="ts">
import TrafficPage from '../../components/TrafficPage.vue';
import EventsPage from '../../components/EventsPage.vue';
/* Instrument skin shell: terminal login, then a numbered side rail and a
 * top bar with real readouts (online count from GET /devices, local UTC
 * clock in dot-matrix). Rail collapses to icons, then to a bottom key bar
 * on phones. */
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { UiIcon, eventOrigin } from '@nyabula/ui';
import './instrument.css';
import DotMatrix from '../../components/DotMatrix.vue';
import InstrumentLogin from './InstrumentLogin.vue';
import InstrumentOverview from './InstrumentOverview.vue';
import InstrumentFleet from './InstrumentFleet.vue';
import InstrumentDevice from './InstrumentDevice.vue';
import InstrumentAccount from './InstrumentAccount.vue';
import { useAuthStore } from '../../stores/auth';
import { useNavStore, type Page } from '../../stores/nav';
import { useDevicesStore } from '../../stores/devices';
import { useAppearance } from '../../logic/useAppearance';
import { handshaking } from './handshake';

const auth = useAuthStore();
const nav = useNavStore();
const devices = useDevicesStore();
const ap = useAppearance();

const PAGES: { id: Page; no: string; label: string; en: string; icon: string }[] = [
  { id: 'overview', no: '01', label: '总览', en: 'OVERVIEW', icon: 'dashboard' },
  { id: 'devices', no: '02', label: '设备舰队', en: 'FLEET', icon: 'devices' },
  { id: 'traffic', no: '03', en: 'TRAFFIC', label: '流量', icon: 'trending_up' },
  { id: 'events', no: '04', en: 'EVENTS', label: '事件', icon: 'history' },
  { id: 'account', no: '05', label: '账号', en: 'ACCOUNT', icon: 'person' },
];
const section = computed<Page>(() => (nav.page === 'device' ? 'devices' : nav.page));
const crumb = computed(() => (nav.page === 'device' ? `设备 / ${nav.deviceId}` : PAGES.find((p) => p.id === section.value)?.label ?? ''));
const online = computed(() => devices.devices.filter((d) => d.online).length);

const now = ref(new Date());
let timer = 0;
onMounted(() => (timer = window.setInterval(() => (now.value = new Date()), 1000)));
onBeforeUnmount(() => clearInterval(timer));
const utc = computed(() => now.value.toISOString().slice(11, 19));
const initials = computed(() => (auth.user?.name || auth.user?.email || '??').slice(0, 2).toUpperCase());
</script>

<template>
  <div class="insc">
    <Transition name="ins-page" mode="out-in">
      <InstrumentLogin v-if="!auth.user || handshaking" key="login" />
      <div v-else key="console" class="console">
        <aside class="rail">
          <div class="brand">
            <DotMatrix text="NYA" :dot="3" :gap="1" />
            <div class="brand-t"><span class="ins-label">NYABULA</span><span class="ins-label ins-sig">CLOUD</span></div>
          </div>
          <nav class="nav">
            <button v-for="p in PAGES" :key="p.id" class="nv" :class="{ on: section === p.id }" :aria-current="section === p.id ? 'page' : undefined" @click="nav.go(p.id)">
              <span class="nv-no">{{ p.no }}</span>
              <UiIcon :name="p.icon" :size="17" class="nv-ic" />
              <span class="nv-lb">{{ p.label }}</span>
              <span class="nv-en ins-label">{{ p.en }}</span>
            </button>
          </nav>
          <div class="rail-foot ins-label">
            <span><span class="ins-led" :class="{ on: online > 0 }" /> {{ online }}/{{ devices.devices.length }} ONLINE</span>
          </div>
        </aside>

        <div class="body">
          <header class="bar">
            <div class="crumb">
              <span class="ins-label">CLOUD /</span>
              <span class="crumb-t">{{ crumb }}</span>
            </div>
            <div class="bar-r">
              <div class="rd"><span class="ins-label">FLEET</span><span class="ins-num" :class="online ? 'ins-sig' : 'ins-dim'">{{ online }}/{{ devices.devices.length }} ON</span></div>
              <div class="rd clk"><span class="ins-label">UTC</span><DotMatrix :text="utc" :dot="2.4" :gap="0.9" /></div>
              <button class="ins-key sq" :title="ap.theme.mode === 'dark' ? '切换浅色' : '切换深色'" @click="ap.toggleMode(eventOrigin($event))">
                <UiIcon :name="ap.theme.mode === 'dark' ? 'dark_mode' : 'light_mode'" :size="16" />
              </button>
              <button class="user" title="账号与外观" @click="nav.go('account')">
                <span class="av">{{ initials }}</span>
                <span class="user-t"><b>{{ auth.user.name || auth.user.email }}</b><span class="ins-label">{{ auth.user.email }}</span></span>
              </button>
            </div>
          </header>
          <main class="main">
            <Transition name="ins-page" mode="out-in">
              <InstrumentOverview v-if="nav.page === 'overview'" key="o" />
              <InstrumentDevice v-else-if="nav.page === 'device' && nav.deviceId" :key="'d' + nav.deviceId" :device-id="nav.deviceId" />
              <TrafficPage v-else-if="nav.page === 'traffic'" key="t" /><EventsPage v-else-if="nav.page === 'events'" key="e" />
              <InstrumentAccount v-else-if="nav.page === 'account'" key="a" />
              <InstrumentFleet v-else key="f" />
            </Transition>
          </main>
        </div>
      </div>
    </Transition>
  </div>
</template>

<style scoped>
.console { display: grid; grid-template-columns: 224px minmax(0, 1fr); min-height: 100dvh; }
.rail {
  position: sticky;
  top: 0;
  height: 100dvh;
  display: flex;
  flex-direction: column;
  gap: 18px;
  padding: 18px 12px;
  background: var(--md-surface-dim);
  border-right: 1px solid var(--c-line);
  z-index: 30;
}
.brand { display: flex; align-items: center; gap: 10px; padding: 0 6px 14px; border-bottom: 1px solid var(--c-line); color: var(--c-fg); }
.brand-t { display: flex; flex-direction: column; gap: 3px; }
.nav { display: flex; flex-direction: column; gap: 2px; flex: 1; }
.nv {
  position: relative;
  display: grid;
  grid-template-columns: 22px 20px 1fr;
  grid-template-rows: auto auto;
  column-gap: 8px;
  align-items: center;
  padding: 9px 10px;
  border: 1px solid transparent;
  border-radius: 3px;
  background: none;
  color: var(--c-dim);
  cursor: pointer;
  text-align: left;
  transition: background 140ms, color 140ms, border-color 140ms;
}
.nv::before { content: ''; position: absolute; left: -12px; top: 8px; bottom: 8px; width: 2px; background: var(--c-sig); transform: scaleY(0); transition: transform 200ms var(--c-ease); }
.nv:hover { color: var(--c-fg); background: var(--c-panel); }
.nv.on { color: var(--c-fg); background: var(--c-panel-2); border-color: var(--c-line); }
.nv.on::before { transform: scaleY(1); }
.nv-no { grid-row: 1 / 3; font: 600 10px var(--c-mono); color: var(--c-mute); }
.nv.on .nv-no { color: var(--c-sig); }
.nv-ic { grid-row: 1 / 3; }
.nv-lb { font: 600 14px var(--font-body); }
.nv-en { grid-column: 3; font-size: 9px; }
.rail-foot { display: flex; flex-direction: column; gap: 6px; padding: 12px 6px 0; border-top: 1px solid var(--c-line); }
.rail-foot span { display: flex; align-items: center; gap: 6px; }
.body { min-width: 0; display: flex; flex-direction: column; }
.bar {
  position: sticky;
  top: 0;
  z-index: 20;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  height: 60px;
  padding: 0 20px;
  background: var(--c-bg);
  border-bottom: 1px solid var(--c-line);
}
.crumb { display: flex; align-items: baseline; gap: 8px; min-width: 0; }
.crumb-t { font: 400 20px var(--font-title); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.bar-r { display: flex; align-items: center; gap: 10px; }
.rd { display: flex; flex-direction: column; gap: 4px; padding: 6px 12px; border: 1px solid var(--c-line); border-radius: 3px; background: var(--c-panel); min-width: 96px; }
.rd .ins-num { font-size: 12px; }
.clk { color: var(--c-fg); }
.sq { width: 40px; padding: 0; }
.user { display: flex; align-items: center; gap: 10px; padding: 4px 10px 4px 4px; border: 1px solid var(--c-line-2); border-radius: 3px; background: none; color: var(--c-fg); cursor: pointer; max-width: 240px; }
.user:hover { border-color: var(--c-dim); }
.av { width: 32px; height: 32px; flex: none; display: grid; place-items: center; background: var(--c-fg); color: var(--c-bg); font: 700 11px var(--c-mono); border-radius: 2px; }
.user-t { display: flex; flex-direction: column; gap: 2px; text-align: left; min-width: 0; }
.user-t b { font-size: 13px; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.user-t .ins-label { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; text-transform: none; letter-spacing: 0.02em; }
.main { flex: 1; min-width: 0; }
@media (max-width: 1100px) { .rd:not(.clk) { display: none; } .user-t { display: none; } .user { padding: 4px; } }
@media (max-width: 980px) {
  .console { grid-template-columns: 64px minmax(0, 1fr); }
  .brand-t, .nv-lb, .nv-en, .rail-foot { display: none; }
  .brand { justify-content: center; padding: 0 0 14px; }
  .nv { grid-template-columns: 1fr; justify-items: center; padding: 10px 0; }
  .nv-no { display: none; }
  .nv-ic { grid-row: auto; }
}
@media (max-width: 720px) {
  .console { grid-template-columns: minmax(0, 1fr); }
  .rail {
    position: fixed;
    top: auto;
    bottom: 0;
    left: 0;
    right: 0;
    height: auto;
    flex-direction: row;
    padding: 6px 6px calc(6px + env(safe-area-inset-bottom));
    border-right: none;
    border-top: 1px solid var(--c-line);
  }
  .brand { display: none; }
  .nav { flex-direction: row; }
  .nv { flex: 1; grid-template-columns: 1fr; padding: 8px 0; }
  .nv::before { display: none; }
  .nv-lb { display: block; font-size: 11px; text-align: center; }
  .bar { padding: 0 12px; height: 54px; }
  .clk { display: none; }
  .main { padding-bottom: 76px; }
}
</style>
