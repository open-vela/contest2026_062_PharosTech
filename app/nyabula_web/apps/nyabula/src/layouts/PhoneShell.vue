<script setup lang="ts">
/* Phone shell: small collapsing app bar, single-column stack with push/pop
 * transitions and the shared five-item primary navigation. */
import { computed, ref, watch } from 'vue';
import { RouterView, useRoute } from 'vue-router';
import { StatusPill, ThemeSwitcher, UiIcon, NyabulaLogo } from '@nyabula/ui';
import { useShell } from '../composables/useShell';
import ConnectionBanner from '../components/ConnectionBanner.vue';

const shell = useShell();
const route = useRoute();
const transitionName = ref('stack-push');
let lastDepth = 0;
watch(
  () => route.fullPath,
  () => {
    const d = shell.depth.value;
    transitionName.value = d < lastDepth ? 'stack-pop' : d > lastDepth ? 'stack-push' : 'fade';
    lastDepth = d;
  },
);
const navItems = shell.nav;
const showBottom = computed(() => shell.depth.value === 0);
</script>

<template>
  <div class="phone">
    <header class="bar">
      <button v-if="shell.depth.value > 0" class="bar-btn" @click="shell.back()"><UiIcon name="arrow_back" :size="22" /></button>
      <NyabulaLogo v-else :size="26" class="bar-logo" />
      <h1 class="bar-title">{{ shell.title.value }}</h1>
      <StatusPill :tone="shell.connTone.value" :label="shell.connLabel.value" class="bar-pill" />
      <ThemeSwitcher compact />
    </header>
    <ConnectionBanner />
    <main class="main" :class="{ 'has-bottom': showBottom }">
      <RouterView v-slot="{ Component }">
        <Transition :name="transitionName">
          <component :is="Component" :key="route.path" class="stack-page" />
        </Transition>
      </RouterView>
    </main>
    <nav v-show="showBottom" class="bottom">
      <button
        v-for="item in navItems"
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
</template>

<style scoped>
.phone { display: flex; flex-direction: column; height: 100dvh; background: var(--md-surface); }
.bar {
  display: flex;
  align-items: center;
  gap: 8px;
  height: calc(56px + var(--safe-t));
  padding: var(--safe-t) 12px 0 12px;
  flex: none;
}
.bar-btn { border: none; background: transparent; color: var(--md-on-surface); width: 40px; height: 40px; border-radius: 50%; display: grid; place-items: center; }
.bar-logo { color: var(--md-primary); margin: 0 8px 0 6px; }
.bar-title { font: 600 18px var(--font-title); color: var(--md-on-surface); margin: 0; flex: 1; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.bar-pill { max-width: 40vw; overflow: hidden; }
.main { flex: 1; min-height: 0; overflow-y: auto; position: relative; }
.stack-page { min-height: 100%; }
.bottom {
  display: grid;
  grid-auto-flow: column;
  grid-auto-columns: 1fr;
  height: calc(var(--shell-bottom) + var(--safe-b));
  padding-bottom: var(--safe-b);
  background: var(--md-surface-container);
  border-top: 1px solid var(--md-outline-variant);
  flex: none;
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
  font: 600 11px var(--font-body);
  cursor: pointer;
  padding: 0;
}
.bottom-item.disabled { opacity: 0.4; }
.bottom-pill { width: 56px; height: 30px; border-radius: 999px; display: grid; place-items: center; transition: background var(--dur-fast), transform var(--dur-fast) var(--ease-spring); }
.bottom-item:active .bottom-pill { transform: scale(0.9); }
.bottom-item.active { color: var(--md-on-surface); }
.bottom-item.active .bottom-pill { background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.more-list { display: flex; flex-direction: column; gap: 8px; }
</style>
