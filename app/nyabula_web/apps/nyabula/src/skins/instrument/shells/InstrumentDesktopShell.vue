<script setup lang="ts">
/* Instrument desktop shell: a top "module key" bar (numbered hardware
 * keys with LEDs, dot-matrix clock), a status strip (back, title, device
 * switcher, link LED, command key, theme, context toggle), main + right
 * context panel. Same capabilities as the classic DesktopShell: primary
 * nav, client settings, back, Ctrl/Cmd+K palette, connection banner and
 * the collapsible #shell-context column. */
import { computed, onBeforeUnmount, onMounted, provide, ref } from 'vue';
import { RouterView } from 'vue-router';
import { StatusPill, ThemeSwitcher, UiIcon, NyabulaLogo } from '@nyabula/ui';
import { useShell } from '../../../composables/useShell';
import ConnectionBanner from '../../../components/ConnectionBanner.vue';
import PanelUpdateBanner from '../../../components/PanelUpdateBanner.vue';
import CommandPalette from '../../../components/CommandPalette.vue';
import DeviceSwitcher from '../../../components/DeviceSwitcher.vue';
import { useContextColumn } from '../../../composables/useContextColumn';
import DotMatrix from '../parts/DotMatrix.vue';
import { useClock } from '../parts/useClock';

const shell = useShell();
const clock = useClock();
const palette = ref(false);
provide('shellHasContext', true);

function onKey(e: KeyboardEvent) {
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
    e.preventDefault();
    palette.value = !palette.value;
  }
  if (e.key === 'Escape') palette.value = false;
}
onMounted(() => window.addEventListener('keydown', onKey));
onBeforeUnmount(() => window.removeEventListener('keydown', onKey));

const ctx = useContextColumn(shell.inDevice);
const contextOpen = ctx.open;
const hasContext = ctx.hasContext;
const toggleContext = ctx.toggle;
const showContext = computed(() => ctx.show.value);
const idx = (i: number) => String(i + 1).padStart(2, '0');
const activeIdx = computed(() => {
  const i = shell.nav.findIndex((n) => n.id === shell.activeNav.value);
  if (i >= 0) return idx(i);
  return shell.route.name === 'settings' ? idx(shell.nav.length) : '--';
});
const clockText = computed(() => clock.hhmm());
</script>

<template>
  <div class="ins-desk" :class="{ 'ctx-open': showContext }">
    <header class="keys">
      <div class="brand">
        <NyabulaLogo :size="28" />
        <span class="brand-name">NYABULA</span>
      </div>
      <nav class="modkeys" aria-label="主导航">
        <button
          v-for="(item, i) in shell.nav"
          :key="item.id"
          class="modkey"
          :class="{ active: shell.activeNav.value === item.id, disabled: item.device && !shell.deviceKey.value }"
          :aria-current="shell.activeNav.value === item.id ? 'page' : undefined"
          :title="item.label"
          @click="shell.go(item)"
        >
          <span class="led" />
          <span class="k-idx">{{ idx(i) }}</span>
          <span class="k-label">{{ item.label }}</span>
        </button>
        <RouterLink class="modkey" :class="{ active: shell.route.name === 'settings' }" to="/settings" title="客户端设置">
          <span class="led" />
          <span class="k-idx">{{ idx(shell.nav.length) }}</span>
          <span class="k-label">客户端</span>
        </RouterLink>
      </nav>
      <div class="clock" :title="clock.now.value.toLocaleString()">
        <DotMatrix :text="clockText" :colon-on="clock.colonOn()" :dot="3" :gap="1.2" />
      </div>
    </header>

    <div class="strip">
      <button v-if="shell.depth.value > 0" class="strip-key" title="返回" @click="shell.back()"><UiIcon name="arrow_back" :size="18" /></button>
      <span class="crumb mono">{{ activeIdx }}</span>
      <h1 class="title">{{ shell.title.value }}</h1>
      <DeviceSwitcher class="strip-switcher" />
      <StatusPill :tone="shell.connTone.value" :label="shell.connLabel.value" />
      <button class="cmd" title="命令面板 (Ctrl+K)" @click="palette = true">
        <UiIcon name="search" :size="16" />
        <span>搜索 / 命令</span>
        <kbd>⌘K</kbd>
      </button>
      <ThemeSwitcher />
      <button v-if="shell.inDevice.value && hasContext" class="strip-key" :class="{ on: contextOpen }" title="侧栏" @click="toggleContext">
        <UiIcon name="dashboard" :size="18" />
      </button>
    </div>
    <ConnectionBanner />
      <PanelUpdateBanner />

    <div class="content">
      <main class="main">
        <RouterView v-slot="{ Component }">
          <Transition name="ins-page" mode="out-in">
            <component :is="Component" />
          </Transition>
        </RouterView>
      </main>
      <aside id="shell-context" class="context" :class="{ hidden: !showContext }" />
    </div>
    <CommandPalette v-model="palette" />
  </div>
</template>

<style scoped>
.ins-desk {
  --mono: var(--ins-mono);
  display: flex;
  flex-direction: column;
  height: 100dvh;
  overflow: hidden;
  background-color: var(--md-surface);
  background-image:
    linear-gradient(var(--ins-grid) 1px, transparent 1px),
    linear-gradient(90deg, var(--ins-grid) 1px, transparent 1px);
  background-size: 24px 24px;
}
.keys {
  display: flex;
  align-items: stretch;
  height: 52px;
  flex: none;
  border-bottom: 1px solid var(--md-outline-variant);
  background: var(--md-surface-container);
}
.brand { display: flex; align-items: center; gap: 10px; padding: 0 18px; border-right: 1px solid var(--md-outline-variant); }
.brand-name { font: 700 12px var(--mono); letter-spacing: 0.3em; color: var(--md-on-surface); }
.modkeys { display: flex; flex: 1; min-width: 0; }
.modkey {
  position: relative;
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 0 18px;
  border: none;
  border-right: 1px solid var(--md-outline-variant);
  background: transparent;
  color: var(--md-on-surface-variant);
  font: 600 13px var(--font-body);
  cursor: pointer;
  text-decoration: none;
  white-space: nowrap;
  transition: background 0.14s, color 0.14s;
}
.modkey:hover { background: var(--md-surface-container-high); color: var(--md-on-surface); }
.modkey:active { transform: translateY(1px); }
.modkey.disabled { opacity: 0.38; }
.modkey.active { color: var(--md-on-surface); background: var(--md-surface); }
.modkey.active::after { content: ''; position: absolute; left: 0; right: 0; bottom: -1px; height: 2px; background: var(--md-primary); }
.led { width: 6px; height: 6px; border-radius: 50%; background: var(--md-outline); transition: background 0.12s steps(2, jump-none), box-shadow 0.12s; }
.modkey.active .led { background: var(--md-primary); box-shadow: var(--ins-led-glow); }
.k-idx { font: 600 10.5px var(--mono); letter-spacing: 0.1em; color: var(--md-outline); }
.modkey.active .k-idx { color: var(--md-on-surface-variant); }
.clock { display: flex; align-items: center; padding: 0 18px; border-left: 1px solid var(--md-outline-variant); color: var(--md-primary); }

.strip {
  display: flex;
  align-items: center;
  gap: 10px;
  height: 50px;
  flex: none;
  padding: 0 16px;
  border-bottom: 1px solid var(--md-outline-variant);
  background: color-mix(in srgb, var(--md-surface) 92%, transparent);
}
.strip-key {
  width: 32px;
  height: 32px;
  display: grid;
  place-items: center;
  border: none;
  border-radius: 3px;
  background: transparent;
  color: var(--md-on-surface-variant);
  box-shadow: inset 0 0 0 1px var(--md-outline-variant);
  cursor: pointer;
}
.strip-key:hover, .strip-key.on { color: var(--md-on-surface); box-shadow: inset 0 0 0 1px var(--md-outline); }
.strip-key.on { box-shadow: inset 0 0 0 1px var(--md-primary); color: var(--md-primary); }
.strip-key:active { transform: translateY(1px); }
.crumb { font: 600 11px var(--mono); color: var(--md-primary); letter-spacing: 0.1em; }
.title { font: 400 18px var(--font-title); color: var(--md-on-surface); margin: 0 8px 0 0; white-space: nowrap; }
.strip-switcher { margin-right: auto; }
.cmd {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 6px 8px 6px 10px;
  border: none;
  border-radius: 3px;
  background: var(--md-surface-container);
  box-shadow: inset 0 0 0 1px var(--md-outline-variant);
  color: var(--md-on-surface-variant);
  font: 600 11.5px var(--mono);
  letter-spacing: 0.04em;
  cursor: pointer;
}
.cmd:hover { color: var(--md-on-surface); box-shadow: inset 0 0 0 1px var(--md-outline); }
.cmd kbd { font: 600 10.5px var(--mono); padding: 1px 5px; border-radius: 2px; background: var(--md-surface-container-highest); }

.content { flex: 1; display: grid; grid-template-columns: 1fr; min-height: 0; overflow: hidden; }
.ctx-open .content { grid-template-columns: 1fr var(--shell-context); }
.main { overflow-y: auto; min-width: 0; min-height: 0; position: relative; }
.context {
  border-left: 1px solid var(--md-outline-variant);
  background: var(--md-surface-container);
  overflow-y: auto;
  padding: 14px;
  display: flex;
  flex-direction: column;
  gap: 12px;
}
.context.hidden { display: none; }

.ins-page-enter-active { transition: opacity 0.22s steps(4, jump-none), transform 0.26s cubic-bezier(0.22, 1, 0.36, 1); }
.ins-page-leave-active { transition: opacity 0.12s steps(2, jump-none); }
.ins-page-enter-from { opacity: 0; transform: translateY(8px); }
.ins-page-leave-to { opacity: 0; }

@media (max-width: 1180px) {
  .modkey { padding: 0 12px; }
  .cmd span { display: none; }
}
</style>
