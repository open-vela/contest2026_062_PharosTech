<script setup lang="ts">
/* Instrument tablet shell. Landscape: numbered module keys across the top
 * (incl. client settings); portrait: the keys move to a bottom key bar and
 * client settings sits in the status strip. No context panel (pages render
 * ContextSlot content inline), same as the classic TabletShell. */
import { computed, inject } from 'vue';
import { RouterView } from 'vue-router';
import { StatusPill, ThemeSwitcher, UiIcon, NyabulaLogo } from '@nyabula/ui';
import { useShell } from '../../../composables/useShell';
import type { useFormFactor } from '../../../composables/useFormFactor';
import ConnectionBanner from '../../../components/ConnectionBanner.vue';
import PanelUpdateBanner from '../../../components/PanelUpdateBanner.vue';
import DeviceSwitcher from '../../../components/DeviceSwitcher.vue';
import DotMatrix from '../parts/DotMatrix.vue';
import { useClock } from '../parts/useClock';

const shell = useShell();
const clock = useClock();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const portrait = computed(() => ff.orientation.value === 'portrait');
const idx = (i: number) => String(i + 1).padStart(2, '0');
const clockText = computed(() => clock.hhmm());
</script>

<template>
  <div class="ins-tab" :class="{ portrait }">
    <header v-if="!portrait" class="keys">
      <div class="brand"><NyabulaLogo :size="26" /></div>
      <nav class="modkeys" aria-label="主导航">
        <button
          v-for="(item, i) in shell.nav"
          :key="item.id"
          class="modkey"
          :class="{ active: shell.activeNav.value === item.id, disabled: item.device && !shell.deviceKey.value }"
          @click="shell.go(item)"
        >
          <span class="led" />
          <span class="k-idx">{{ idx(i) }}</span>
          <span class="k-label">{{ item.label }}</span>
        </button>
        <RouterLink class="modkey" :class="{ active: shell.route.name === 'settings' }" to="/settings">
          <span class="led" />
          <span class="k-idx">{{ idx(shell.nav.length) }}</span>
          <span class="k-label">客户端</span>
        </RouterLink>
      </nav>
      <div class="clock"><DotMatrix :text="clockText" :colon-on="clock.colonOn()" :dot="3" :gap="1.2" /></div>
    </header>

    <div class="strip">
      <button v-if="shell.depth.value > 0" class="strip-key" aria-label="返回" @click="shell.back()"><UiIcon name="arrow_back" :size="20" /></button>
      <NyabulaLogo v-else-if="portrait" :size="24" />
      <h1 class="title">{{ shell.title.value }}</h1>
      <DeviceSwitcher compact class="strip-switcher" />
      <StatusPill :tone="shell.connTone.value" :label="shell.connLabel.value" />
      <ThemeSwitcher compact />
      <RouterLink v-if="portrait" class="strip-key" to="/settings" aria-label="客户端设置"><UiIcon name="settings" :size="20" /></RouterLink>
    </div>
    <ConnectionBanner />
      <PanelUpdateBanner />

    <main class="main">
      <RouterView v-slot="{ Component }">
        <Transition name="ins-page" mode="out-in">
          <component :is="Component" />
        </Transition>
      </RouterView>
    </main>

    <nav v-if="portrait" class="bottom" aria-label="主导航">
      <button
        v-for="(item, i) in shell.nav"
        :key="item.id"
        class="bkey"
        :class="{ active: shell.activeNav.value === item.id, disabled: item.device && !shell.deviceKey.value }"
        @click="shell.go(item)"
      >
        <span class="bkey-top"><span class="led" /><span class="k-idx">{{ idx(i) }}</span></span>
        <UiIcon :name="item.icon" :size="20" />
        <span class="bkey-label">{{ item.label }}</span>
      </button>
    </nav>
  </div>
</template>

<style scoped>
.ins-tab {
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
.keys { display: flex; align-items: stretch; height: 50px; flex: none; border-bottom: 1px solid var(--md-outline-variant); background: var(--md-surface-container); padding-top: var(--safe-t); }
.brand { display: flex; align-items: center; padding: 0 14px; border-right: 1px solid var(--md-outline-variant); }
.modkeys { display: flex; flex: 1; min-width: 0; overflow-x: auto; scrollbar-width: none; }
.modkeys::-webkit-scrollbar { display: none; }
.modkey {
  position: relative;
  display: flex;
  align-items: center;
  gap: 7px;
  padding: 0 13px;
  border: none;
  border-right: 1px solid var(--md-outline-variant);
  background: transparent;
  color: var(--md-on-surface-variant);
  font: 600 13px var(--font-body);
  text-decoration: none;
  white-space: nowrap;
  cursor: pointer;
}
.modkey.disabled { opacity: 0.38; }
.modkey.active { color: var(--md-on-surface); background: var(--md-surface); }
.modkey.active::after { content: ''; position: absolute; left: 0; right: 0; bottom: -1px; height: 2px; background: var(--md-primary); }
.led { width: 6px; height: 6px; border-radius: 50%; background: var(--md-outline); flex: none; }
.active .led { background: var(--md-primary); box-shadow: var(--ins-led-glow); }
.k-idx { font: 600 10.5px var(--mono); letter-spacing: 0.1em; color: var(--md-outline); }
.clock { display: flex; align-items: center; padding: 0 14px; border-left: 1px solid var(--md-outline-variant); color: var(--md-primary); }

.strip { display: flex; align-items: center; gap: 10px; min-height: 52px; padding: 0 14px; flex: none; border-bottom: 1px solid var(--md-outline-variant); }
.portrait .strip { padding-top: var(--safe-t); }
.strip-key {
  width: 36px;
  height: 36px;
  display: grid;
  place-items: center;
  border: none;
  border-radius: 3px;
  background: transparent;
  color: var(--md-on-surface);
  box-shadow: inset 0 0 0 1px var(--md-outline-variant);
  cursor: pointer;
}
.title { font: 400 18px var(--font-title); color: var(--md-on-surface); margin: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.strip-switcher { margin-left: auto; }
.main { flex: 1; overflow-y: auto; min-height: 0; position: relative; }

.bottom {
  display: grid;
  grid-auto-flow: column;
  grid-auto-columns: 1fr;
  height: calc(var(--shell-bottom) + var(--safe-b));
  padding-bottom: var(--safe-b);
  flex: none;
  background: var(--md-surface-container);
  border-top: 1px solid var(--md-outline-variant);
}
.bkey {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 2px;
  border: none;
  border-right: 1px solid var(--md-outline-variant);
  background: transparent;
  color: var(--md-on-surface-variant);
  font: 600 11px var(--font-body);
  cursor: pointer;
}
.bkey:last-child { border-right: none; }
.bkey:active { transform: translateY(1px); }
.bkey.disabled { opacity: 0.38; }
.bkey.active { color: var(--md-on-surface); background: var(--md-surface); }
.bkey-top { display: flex; align-items: center; gap: 4px; }

.ins-page-enter-active { transition: opacity 0.22s steps(4, jump-none), transform 0.26s cubic-bezier(0.22, 1, 0.36, 1); }
.ins-page-leave-active { transition: opacity 0.12s steps(2, jump-none); }
.ins-page-enter-from { opacity: 0; transform: translateY(8px); }
.ins-page-leave-to { opacity: 0; }
</style>
