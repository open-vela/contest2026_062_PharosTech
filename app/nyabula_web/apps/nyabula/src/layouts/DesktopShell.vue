<script setup lang="ts">
/* Desktop shell: left nav rail (expandable), top app bar with device
 * switcher + connection + command palette + theme, main + right context
 * panel (collapsible). Keyboard: Ctrl/Cmd+K opens the command palette. */
import { computed, onBeforeUnmount, onMounted, provide, ref } from 'vue';
import { RouterView } from 'vue-router';
import { StatusPill, ThemeSwitcher, UiIcon, NyabulaLogo } from '@nyabula/ui';
import { useShell } from '../composables/useShell';
import ConnectionBanner from '../components/ConnectionBanner.vue';
import CommandPalette from '../components/CommandPalette.vue';
import DeviceSwitcher from '../components/DeviceSwitcher.vue';
import { contextPresence } from '../composables/contextPresence';

const shell = useShell();
const RAIL_KEY = 'nyabula.railWide';
const CTX_KEY = 'nyabula.contextOpen';
const railWide = ref(localStorage.getItem(RAIL_KEY) === '1');
const contextOpen = ref(localStorage.getItem(CTX_KEY) !== '0');
const palette = ref(false);
provide('shellHasContext', true);

function toggleRail() {
  railWide.value = !railWide.value;
  localStorage.setItem(RAIL_KEY, railWide.value ? '1' : '0');
}
function toggleContext() {
  contextOpen.value = !contextOpen.value;
  localStorage.setItem(CTX_KEY, contextOpen.value ? '1' : '0');
}
function onKey(e: KeyboardEvent) {
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
    e.preventDefault();
    palette.value = !palette.value;
  }
  if (e.key === 'Escape') palette.value = false;
}
onMounted(() => window.addEventListener('keydown', onKey));
onBeforeUnmount(() => window.removeEventListener('keydown', onKey));

/* Column is only worth space when a page contributes to it. The target
 * element stays mounted (hidden) so slots can still find it. */
const hasContext = computed(() => contextPresence.count > 0);
const showContext = computed(() => shell.inDevice.value && contextOpen.value && hasContext.value);
</script>

<template>
  <div class="desktop" :class="{ 'rail-wide': railWide, 'ctx-open': showContext }">
    <aside class="rail">
      <button class="rail-logo" title="Nyabula" @click="toggleRail">
        <span class="rail-logo-cell"><NyabulaLogo :size="36" /></span>
        <span class="rail-brand">Nyabula</span>
      </button>
      <nav class="rail-nav">
        <button
          v-for="item in shell.nav"
          :key="item.id"
          class="rail-item"
          :class="{ active: shell.activeNav.value === item.id, disabled: item.device && !shell.deviceKey.value }"
          :aria-current="shell.activeNav.value === item.id ? 'page' : undefined"
          :title="item.label"
          @click="shell.go(item)"
        >
          <span class="rail-pill"><UiIcon :name="item.icon" :size="26" /></span>
          <span class="rail-label">{{ item.label }}</span>
        </button>
      </nav>
      <div class="rail-foot">
        <RouterLink class="rail-item" :class="{ active: shell.route.name === 'settings' }" to="/settings" title="客户端设置">
          <span class="rail-pill"><UiIcon name="computer" :size="26" /></span>
          <span class="rail-label">客户端</span>
        </RouterLink>
        <button class="rail-item" title="展开 / 收起" @click="toggleRail">
          <span class="rail-pill"><UiIcon :name="railWide ? 'chevron_left' : 'chevron_right'" :size="26" /></span>
          <span class="rail-label">收起</span>
        </button>
      </div>
    </aside>

    <div class="body">
      <header class="bar">
        <button v-if="shell.depth.value > 0" class="bar-back" title="返回" @click="shell.back()"><UiIcon name="arrow_back" :size="20" /></button>
        <h1 class="bar-title">{{ shell.title.value }}</h1>
        <DeviceSwitcher class="bar-switcher" />
        <StatusPill :tone="shell.connTone.value" :label="shell.connLabel.value" />
        <button class="bar-search" title="命令面板 (Ctrl+K)" @click="palette = true">
          <UiIcon name="search" :size="18" />
          <span>搜索 / 命令</span>
          <kbd>⌘K</kbd>
        </button>
        <ThemeSwitcher />
        <button v-if="shell.inDevice.value && hasContext" class="bar-icon" :class="{ on: contextOpen }" title="侧栏" @click="toggleContext">
          <UiIcon name="dashboard" :size="20" />
        </button>
      </header>
      <ConnectionBanner />
      <div class="content">
        <main class="main">
          <RouterView v-slot="{ Component }">
            <Transition name="page" mode="out-in">
              <component :is="Component" />
            </Transition>
          </RouterView>
        </main>
        <aside id="shell-context" class="context" :class="{ hidden: !showContext }" />
      </div>
    </div>
    <CommandPalette v-model="palette" />
  </div>
</template>

<style scoped>
.desktop {
  display: grid;
  grid-template-columns: var(--shell-rail) 1fr;
  height: 100vh;
  height: 100dvh;
  background: var(--md-surface);
  overflow: hidden;
  transition: grid-template-columns var(--dur) var(--ease-out);
}
.desktop.rail-wide { grid-template-columns: var(--shell-rail-wide) 1fr; }
.rail {
  display: flex;
  flex-direction: column;
  align-items: stretch;
  padding: 12px 10px;
  gap: 6px;
  background: var(--md-surface-container);
  border-right: 1px solid var(--md-outline-variant);
  overflow: hidden;
}
/* Every rail row is: [fixed 52px cell][label that grows from 0]. The cell is
 * centred in the collapsed 84px rail and hugs the left edge when expanded. */
.rail-logo,
.rail-item {
  display: flex;
  align-items: center;
  justify-content: flex-start;
  gap: 12px;
  padding: 0 0 0 6px;
  border: none;
  background: transparent;
  color: var(--md-on-surface-variant);
  border-radius: 14px;
  cursor: pointer;
  text-decoration: none;
  white-space: nowrap;
  transition: background var(--dur-fast), color var(--dur-fast);
}
.rail-logo { margin-bottom: 8px; }
.rail-pill,
.rail-logo-cell {
  width: 52px;
  height: 52px;
  border-radius: 14px;
  display: grid;
  place-items: center;
  flex: none;
  transition: background var(--dur-fast), transform var(--dur-fast) var(--ease-spring);
}
.rail-item:hover { color: var(--md-on-surface); }
.rail-item:hover .rail-pill { background: var(--md-surface-container-high); }
.rail-wide .rail-item:hover { background: var(--md-surface-container-high); }
.rail-wide .rail-item:hover .rail-pill { background: transparent; }
.rail-item.active { color: var(--md-on-surface); }
.rail-item.active .rail-pill { background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.rail-item:active .rail-pill { transform: scale(0.94); }
.rail-item.disabled { opacity: 0.4; }
.rail-brand,
.rail-label {
  font: 600 15px var(--font-body);
  color: var(--md-on-surface);
  max-width: 0;
  opacity: 0;
  overflow: hidden;
  transform: translateX(-6px);
  transition: max-width var(--dur) var(--ease-out), opacity var(--dur-fast) ease, transform var(--dur) var(--ease-out);
}
.rail-brand { font: 600 19px/1.4 var(--font-title); padding: 2px 0; }
.rail-label { color: inherit; }
.rail-wide .rail-brand,
.rail-wide .rail-label {
  max-width: 130px;
  opacity: 1;
  transform: none;
  transition: max-width var(--dur) var(--ease-out), opacity var(--dur) ease 0.08s, transform var(--dur) var(--ease-out);
}
.rail-nav { display: flex; flex-direction: column; gap: 4px; flex: 1; }
.rail-foot { display: flex; flex-direction: column; gap: 4px; }
.body { display: flex; flex-direction: column; min-width: 0; min-height: 0; height: 100%; }
.bar {
  height: var(--shell-bar);
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 0 18px;
  border-bottom: 1px solid var(--md-outline-variant);
  background: color-mix(in srgb, var(--md-surface) 85%, transparent);
  backdrop-filter: blur(12px);
  -webkit-backdrop-filter: blur(12px);
}
.bar-back, .bar-icon {
  border: none;
  background: transparent;
  color: var(--md-on-surface-variant);
  width: 36px;
  height: 36px;
  border-radius: 50%;
  display: grid;
  place-items: center;
  cursor: pointer;
}
.bar-back:hover, .bar-icon:hover, .bar-icon.on { background: var(--md-surface-container-high); color: var(--md-on-surface); }
.bar-title { font: 600 18px var(--font-title); color: var(--md-on-surface); margin: 0 8px 0 0; }
.bar-switcher { margin-right: auto; }
.bar-search {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  border: 1px solid var(--md-outline-variant);
  background: var(--md-surface-container);
  color: var(--md-on-surface-variant);
  padding: 6px 10px 6px 12px;
  border-radius: 999px;
  cursor: pointer;
  font: 500 13px var(--font-body);
}
.bar-search kbd {
  font: 600 11px ui-monospace, monospace;
  padding: 1px 6px;
  border-radius: 6px;
  background: var(--md-surface-container-highest);
  color: var(--md-on-surface-variant);
}
.content { flex: 1; display: grid; grid-template-columns: 1fr; min-height: 0; overflow: hidden; }
.ctx-open .content { grid-template-columns: 1fr var(--shell-context); }
.main { overflow-y: auto; min-width: 0; min-height: 0; position: relative; }
.context {
  border-left: 1px solid var(--md-outline-variant);
  background: var(--md-surface-container);
  overflow-y: auto;
  padding: 16px;
  display: flex;
  flex-direction: column;
  gap: 14px;
}
.context.hidden { display: none; }
</style>
