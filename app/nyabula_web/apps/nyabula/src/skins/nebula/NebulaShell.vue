<script setup lang="ts">
/* Nebula shell for every form factor (design-lab A): aurora backdrop,
 * floating glass pills on top (back, device + live status, title |
 * search, theme, context toggle, client settings) and a floating Dock with
 * a sliding light for the five destinations.
 * Parity with layouts/{Desktop,Tablet,Phone}Shell.vue: nav + disabled
 * state, /settings entry, back at depth > 0, title, device switcher,
 * connection status, theme switcher, Ctrl/Cmd+K palette (desktop/tablet),
 * ConnectionBanner, desktop context column (#shell-context, collapsible,
 * remembered), phone push/pop stack with the dock hidden on detail pages. */
import { computed, inject, onBeforeUnmount, onMounted, provide, ref, watch } from 'vue';
import { RouterView, useRoute, useRouter } from 'vue-router';
import { setNebulaRouteIntent, ThemeSwitcher, UiIcon } from '@nyabula/ui';
import { useShell } from '../../composables/useShell';
import type { NavItem } from '../../nav';
import type { useFormFactor } from '../../composables/useFormFactor';
import { useContextColumn } from '../../composables/useContextColumn';
import ConnectionBanner from '../../components/ConnectionBanner.vue';
import PanelUpdateBanner from '../../components/PanelUpdateBanner.vue';
import CommandPalette from '../../components/CommandPalette.vue';
import NbAurora from './parts/NbAurora.vue';
import NbDevicePill from './parts/NbDevicePill.vue';
import './nb.css';

const shell = useShell();
const route = useRoute();
const main = ref<HTMLElement | null>(null);
const resetScroll = () => main.value?.scrollTo({ top: 0 });
watch(() => route.fullPath, resetScroll, { flush: 'post' });
const router = useRouter();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const form = computed(() => ff.formFactor.value);
const phone = computed(() => form.value === 'phone');
const desktop = computed(() => form.value === 'desktop');

/* Desktop context column (same storage key as the classic shell). */
provide('shellHasContext', true);
const ctx = useContextColumn(shell.inDevice);
const contextOpen = ctx.open;
const hasContext = ctx.hasContext;
const toggleContext = ctx.toggle;
const showContext = computed(() => desktop.value && ctx.show.value);

/* Command palette. */
const palette = ref(false);
function onKey(e: KeyboardEvent) {
  if (phone.value) return;
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
    e.preventDefault();
    palette.value = !palette.value;
  }
  if (e.key === 'Escape') palette.value = false;
}
onMounted(() => window.addEventListener('keydown', onKey));
onBeforeUnmount(() => window.removeEventListener('keydown', onKey));

/* Dock light follows the destination: it jumps on tap (before the route
 * curtain has covered and the route actually changes). */
const pendingNav = ref<string | null>(null);
const navIndex = computed(() => shell.nav.findIndex((n) => n.id === (pendingNav.value ?? shell.activeNav.value)));
function onDock(item: NavItem, e: MouseEvent) {
  const disabled = item.device && !shell.deviceKey.value;
  if (!disabled && item.id !== shell.activeNav.value) {
    const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
    setNebulaRouteIntent(item.label, { x: r.left + r.width / 2, y: r.top + r.height / 2 });
    pendingNav.value = item.id;
  }
  shell.go(item);
}
const offAfter = router.afterEach(() => (pendingNav.value = null));
const offError = router.onError(() => (pendingNav.value = null));
onBeforeUnmount(() => {
  offAfter();
  offError();
});
const showDock = computed(() => !phone.value || shell.depth.value === 0);

/* Phone: push/pop stack by route depth; larger screens blur-rise. */
const stackName = ref('stack-push');
let lastDepth = shell.depth.value;
watch(
  () => route.fullPath,
  () => {
    const d = shell.depth.value;
    stackName.value = d < lastDepth ? 'stack-pop' : d > lastDepth ? 'stack-push' : 'fade';
    lastDepth = d;
  },
);
const settingsActive = computed(() => route.name === 'settings');
</script>

<template>
  <div class="nb-shell" :class="[form, { 'ctx-open': showContext, 'no-dock': !showDock }]">
    <NbAurora />

    <header class="top">
      <div class="cluster left">
        <Transition name="nb-pop">
          <button v-if="shell.depth.value > 0" class="round nb-glass" title="返回" @click="shell.back()">
            <UiIcon name="arrow_back" :size="20" />
          </button>
        </Transition>
        <NbDevicePill v-if="!phone || shell.depth.value === 0" :compact="phone" />
        <Transition name="nb-title" mode="out-in">
          <h1 v-if="!phone || shell.depth.value > 0" :key="shell.title.value" class="title nb-title">{{ shell.title.value }}</h1>
        </Transition>
      </div>
      <div class="cluster right">
        <button v-if="!phone" class="search nb-glass" :class="{ icon: !desktop }" title="命令面板 (Ctrl+K)" @click="palette = true">
          <UiIcon name="search" :size="17" />
          <template v-if="desktop"><span class="search-text">搜索或输入命令</span><kbd>⌘K</kbd></template>
        </button>
        <ThemeSwitcher class="theme" :compact="!desktop" />
        <button v-if="desktop && shell.inDevice.value && hasContext" class="round nb-glass" :class="{ on: contextOpen }" title="侧栏" @click="toggleContext">
          <UiIcon name="dashboard" :size="19" />
        </button>
        <button class="round nb-glass" :class="{ on: settingsActive }" title="客户端设置" @click="router.push('/settings')">
          <UiIcon name="tune" :size="19" />
        </button>
      </div>
    </header>

    <div class="body">
      <main ref="main" class="main" data-nb-main>
        <div class="banner"><ConnectionBanner />
      <PanelUpdateBanner /></div>
        <RouterView v-slot="{ Component }">
          <Transition v-if="phone" :name="stackName" @after-enter="resetScroll">
            <component :is="Component" :key="route.path" class="stack-page" />
          </Transition>
          <Transition v-else name="nb-page" mode="out-in" @after-enter="resetScroll">
            <component :is="Component" />
          </Transition>
        </RouterView>
      </main>
      <aside v-if="desktop" id="shell-context" class="context nb-glass" :class="{ hidden: !showContext }" />
    </div>

    <nav class="dock nb-glass" :style="{ '--n': shell.nav.length, '--i': Math.max(0, navIndex) }" aria-label="主导航">
      <span class="dock-light" :class="{ off: navIndex < 0 }" />
      <button
        v-for="item in shell.nav"
        :key="item.id"
        class="dock-item"
        :class="{ on: (pendingNav ?? shell.activeNav.value) === item.id, disabled: item.device && !shell.deviceKey.value }"
        :aria-current="shell.activeNav.value === item.id ? 'page' : undefined"
        @click="onDock(item, $event)"
      >
        <UiIcon :name="item.icon" :size="22" />
        <span>{{ item.label }}</span>
      </button>
    </nav>

    <CommandPalette v-if="!phone" v-model="palette" />
  </div>
</template>

<style scoped>
.nb-shell {
  --top-h: 84px;
  --dock-clear: 124px;
  position: relative;
  height: 100dvh;
  overflow: hidden;
  isolation: isolate;
  color: var(--nb-fg);
}
.nb-shell.phone { --top-h: 68px; --dock-clear: calc(96px + var(--safe-b)); --shell-bottom: 84px; }

/* ---------- top pills ---------- */
.top {
  position: absolute;
  z-index: 30;
  top: 0;
  left: 0;
  right: 0;
  height: var(--top-h);
  padding: calc(16px + var(--safe-t)) clamp(16px, 3vw, 36px) 0;
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 12px;
  pointer-events: none;
  background: linear-gradient(to bottom, color-mix(in srgb, var(--md-surface) 72%, transparent) 20%, transparent);
}
.phone .top { padding: calc(10px + var(--safe-t)) 12px 0; }
.cluster { display: flex; align-items: center; gap: 10px; min-width: 0; pointer-events: auto; }
.cluster.left { flex: 1; }
.title { margin: 0 0 0 6px; font-size: 19px; color: var(--nb-fg); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.phone .title { margin: 0; font-size: 18px; }
.round {
  width: 44px;
  height: 44px;
  flex: none;
  border-radius: 50%;
  display: grid;
  place-items: center;
  color: var(--nb-sub);
  cursor: pointer;
  padding: 0;
  transition: color 0.2s, transform 0.35s var(--nb-spring);
}
.round:hover, .round.on { color: var(--nb-fg); }
.round.on { box-shadow: inset 0 1px 0 var(--nb-hi), 0 0 0 1px rgba(var(--nb-acc-rgb), 0.45), 0 0 22px -6px rgba(var(--nb-acc-rgb), 0.7); color: var(--nb-acc); }
.round:active { transform: scale(0.92); }
.phone .round { width: 42px; height: 42px; }
.search {
  display: flex;
  align-items: center;
  gap: 10px;
  height: 44px;
  min-width: 260px;
  padding: 0 12px 0 16px;
  border-radius: 999px;
  color: var(--nb-sub);
  cursor: pointer;
  font: 500 13.5px var(--font-body);
  transition: color 0.2s, transform 0.35s var(--nb-spring);
}
.search.icon { min-width: 0; width: 44px; padding: 0; justify-content: center; }
.search:hover { color: var(--nb-fg); }
.search:active { transform: scale(0.98); }
.search-text { flex: 1; text-align: left; }
.search kbd { font: 600 11px ui-monospace, monospace; padding: 2px 7px; border-radius: 7px; background: var(--nb-soft-2); color: var(--nb-sub); }
.theme { height: 44px; }

/* ---------- body ---------- */
.body { position: relative; z-index: 1; height: 100%; display: grid; grid-template-columns: minmax(0, 1fr); }
.ctx-open .body { grid-template-columns: minmax(0, 1fr) var(--shell-context); }
.main {
  position: relative;
  min-width: 0;
  min-height: 0;
  overflow-y: auto;
  overflow-x: hidden;
  padding-top: var(--top-h);
  padding-bottom: var(--dock-clear);
}
/* Keep the desktop scroll rail below the header, with a separate gutter
 * before the centered context panel. The panel keeps its original width. */
.desktop .main { margin-top: var(--top-h); padding-top: 0; }
.nb-shell .main::-webkit-scrollbar { width: 5px; }
.nb-shell .main::-webkit-scrollbar-track { background: transparent; }
.nb-shell .main::-webkit-scrollbar-thumb { background: var(--nb-line); border-radius: 999px; }
.nb-shell .main:hover::-webkit-scrollbar-thumb,
.nb-shell .main:focus-within::-webkit-scrollbar-thumb { background: var(--nb-line-strong); }
.phone .main { padding-bottom: 0; }
.phone .main :deep(.stack-page) { padding-bottom: var(--dock-clear); }
.phone.no-dock .main :deep(.stack-page) { padding-bottom: calc(24px + var(--safe-b)); }
.banner { padding: 0 clamp(16px, 3vw, 36px); }
.banner:empty { display: none; }
.stack-page { min-height: 100%; }
.context {
  margin: var(--top-h) clamp(8px, 1vw, 14px) 28px;
  padding: 16px;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
  gap: 14px;
  animation: nb-ctx-in 0.6s var(--nb-ease) both;
}
.context.hidden { display: none; }
@keyframes nb-ctx-in { from { opacity: 0; transform: translateX(24px); } }
/* Cards inside the glass column: no second blur layer. */
.context :deep(.md-card) {
  -webkit-backdrop-filter: none;
  backdrop-filter: none;
  background: var(--nb-soft);
  box-shadow: none;
  padding: 16px 18px;
}

/* ---------- dock ---------- */
.dock {
  position: absolute;
  z-index: 40;
  left: 50%;
  bottom: calc(22px + var(--safe-b));
  transform: translateX(-50%);
  display: grid;
  grid-template-columns: repeat(var(--n), 76px);
  padding: 8px;
  border-radius: 28px;
  animation: nb-dock-in 0.9s var(--nb-ease) 0.2s both;
  transition: transform 0.5s var(--nb-ease), opacity 0.3s;
}
@keyframes nb-dock-in { from { opacity: 0; transform: translate(-50%, 30px) scale(0.94); } }
.dock-light {
  position: absolute;
  top: 8px;
  left: 8px;
  width: 76px;
  height: calc(100% - 16px);
  border-radius: 20px;
  background: radial-gradient(90% 90% at 50% 100%, rgba(var(--nb-acc-rgb), 0.35), rgba(var(--nb-acc-rgb), 0.08) 70%);
  border: 1px solid rgba(var(--nb-acc-rgb), 0.25);
  box-shadow: 0 0 30px -6px rgba(var(--nb-acc-rgb), 0.6), inset 0 1px 0 rgba(255, 255, 255, 0.15);
  transform: translateX(calc(var(--i) * 100%));
  transition: transform 0.6s var(--nb-spring), opacity 0.3s;
}
.dock-light.off { opacity: 0; }
.dock-item {
  position: relative;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 4px;
  padding: 9px 0 7px;
  border: none;
  background: transparent;
  color: var(--nb-sub);
  font: 600 11.5px var(--font-body);
  cursor: pointer;
  transition: color 0.25s, transform 0.35s var(--nb-spring);
}
.dock-item:hover { color: var(--nb-fg); transform: translateY(-2px); }
.dock-item:active { transform: scale(0.92); }
.dock-item.on { color: var(--nb-fg); }
.dock-item.on :deep(.ui-icon) { color: var(--nb-acc); filter: drop-shadow(0 0 6px rgba(var(--nb-acc-rgb), 0.7)); }
.dock-item.disabled { opacity: 0.38; }

.phone .dock {
  left: 10px;
  right: 10px;
  bottom: calc(10px + var(--safe-b));
  transform: none;
  grid-template-columns: repeat(var(--n), 1fr);
  border-radius: 24px;
  padding: 6px;
  animation: none;
}
.phone .dock-light { top: 6px; left: 6px; width: calc((100% - 12px) / var(--n)); height: calc(100% - 12px); border-radius: 18px; }
.phone .dock-item { font-size: 10.5px; padding: 8px 0 6px; }
.phone.no-dock .dock { transform: translateY(140%); opacity: 0; pointer-events: none; }

.nb-title-enter-active { transition: opacity 0.35s, transform 0.45s var(--nb-ease); }
.nb-title-leave-active { transition: opacity 0.15s; }
.nb-title-enter-from { opacity: 0; transform: translateY(8px); }
.nb-title-leave-to { opacity: 0; }

@media (max-width: 1180px) {
  .desktop .search { min-width: 0; width: 44px; padding: 0; justify-content: center; }
  .desktop .search-text, .desktop .search kbd { display: none; }
}
</style>
