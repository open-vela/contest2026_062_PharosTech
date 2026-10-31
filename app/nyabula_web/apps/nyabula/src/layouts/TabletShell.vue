<script setup lang="ts">
/* Tablet shell: landscape = compact left rail; portrait = bottom bar.
 * Compact app bar; no context panel (pages use master-detail internally). */
import { computed, inject } from 'vue';
import { RouterView } from 'vue-router';
import { StatusPill, ThemeSwitcher, UiIcon, NyabulaLogo } from '@nyabula/ui';
import { useShell } from '../composables/useShell';
import type { useFormFactor } from '../composables/useFormFactor';
import ConnectionBanner from '../components/ConnectionBanner.vue';
import PanelUpdateBanner from '../components/PanelUpdateBanner.vue';
import DeviceSwitcher from '../components/DeviceSwitcher.vue';

const shell = useShell();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const portrait = computed(() => ff.orientation.value === 'portrait');
</script>

<template>
  <div class="tablet" :class="{ portrait }">
    <aside v-if="!portrait" class="rail">
      <div class="rail-logo"><NyabulaLogo :size="30" /></div>
      <nav class="rail-nav">
        <button
          v-for="item in shell.nav"
          :key="item.id"
          class="rail-item"
          :class="{ active: shell.activeNav.value === item.id, disabled: item.device && !shell.deviceKey.value }"
          @click="shell.go(item)"
        >
          <span class="rail-pill"><UiIcon :name="item.icon" :size="22" /></span>
          <span class="rail-label">{{ item.label }}</span>
        </button>
      </nav>
      <RouterLink class="rail-item" :class="{ active: shell.route.name === 'settings' }" to="/settings">
        <span class="rail-pill"><UiIcon name="computer" :size="22" /></span>
        <span class="rail-label">客户端</span>
      </RouterLink>
    </aside>

    <div class="body">
      <header class="bar">
        <button v-if="shell.depth.value > 0" class="bar-back" @click="shell.back()"><UiIcon name="arrow_back" :size="22" /></button>
        <h1 class="bar-title">{{ shell.title.value }}</h1>
        <DeviceSwitcher compact class="bar-switcher" />
        <StatusPill :tone="shell.connTone.value" :label="shell.connLabel.value" />
        <ThemeSwitcher compact />
        <RouterLink v-if="portrait" class="bar-icon" to="/settings" title="客户端设置"><UiIcon name="computer" :size="22" /></RouterLink>
      </header>
      <ConnectionBanner />
      <PanelUpdateBanner />
      <main class="main">
        <RouterView v-slot="{ Component }">
          <Transition name="page" mode="out-in">
            <component :is="Component" />
          </Transition>
        </RouterView>
      </main>
      <nav v-if="portrait" class="bottom">
        <button
          v-for="item in shell.nav"
          :key="item.id"
          class="bottom-item"
          :class="{ active: shell.activeNav.value === item.id, disabled: item.device && !shell.deviceKey.value }"
          @click="shell.go(item)"
        >
          <span class="bottom-pill"><UiIcon :name="item.icon" :size="22" /></span>
          <span>{{ item.label }}</span>
        </button>
      </nav>
    </div>
  </div>
</template>

<style scoped>
.tablet { display: grid; grid-template-columns: 72px 1fr; height: 100dvh; background: var(--md-surface); overflow: hidden; }
.tablet.portrait { grid-template-columns: 1fr; }
.rail {
  display: flex;
  flex-direction: column;
  align-items: center;
  padding: 14px 6px;
  gap: 8px;
  background: var(--md-surface-container);
  border-right: 1px solid var(--md-outline-variant);
}
.rail-logo { color: var(--md-primary); padding: 8px; margin-bottom: 6px; }
.rail-nav { display: flex; flex-direction: column; gap: 8px; flex: 1; }
.rail-item {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 3px;
  border: none;
  background: transparent;
  color: var(--md-on-surface-variant);
  cursor: pointer;
  text-decoration: none;
  padding: 2px;
}
.rail-item.disabled { opacity: 0.4; }
.rail-pill { width: 52px; height: 32px; border-radius: 999px; display: grid; place-items: center; transition: background var(--dur-fast); }
.rail-item.active { color: var(--md-on-surface); }
.rail-item.active .rail-pill { background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.rail-label { font: 600 11px var(--font-body); }
.body { display: flex; flex-direction: column; min-width: 0; min-height: 0; height: 100%; }
.bar {
  height: var(--shell-bar);
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 0 14px;
  padding-top: var(--safe-t);
  border-bottom: 1px solid var(--md-outline-variant);
}
.bar-back, .bar-icon {
  border: none;
  background: transparent;
  color: var(--md-on-surface);
  width: 40px;
  height: 40px;
  border-radius: 50%;
  display: grid;
  place-items: center;
  cursor: pointer;
}
.bar-title { font: 600 18px var(--font-title); color: var(--md-on-surface); margin: 0; }
.bar-switcher { margin-left: auto; }
.main { flex: 1; overflow-y: auto; min-height: 0; position: relative; }
.bottom {
  display: grid;
  grid-auto-flow: column;
  grid-auto-columns: 1fr;
  height: calc(var(--shell-bottom) + var(--safe-b));
  padding-bottom: var(--safe-b);
  background: var(--md-surface-container);
  border-top: 1px solid var(--md-outline-variant);
}
.bottom-item {
  border: none;
  background: transparent;
  color: var(--md-on-surface-variant);
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 2px;
  font: 600 11.5px var(--font-body);
  cursor: pointer;
}
.bottom-item.disabled { opacity: 0.4; }
.bottom-pill { width: 60px; height: 30px; border-radius: 999px; display: grid; place-items: center; transition: background var(--dur-fast); }
.bottom-item.active { color: var(--md-on-surface); }
.bottom-item.active .bottom-pill { background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
</style>
