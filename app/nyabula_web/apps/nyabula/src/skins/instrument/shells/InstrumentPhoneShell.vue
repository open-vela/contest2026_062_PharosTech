<script setup lang="ts">
/* Instrument phone shell: compact status bar, single-column stack with
 * push/pop transitions and a numbered bottom key bar that hides on detail
 * pages (depth > 0). Mirrors the classic PhoneShell. */
import { computed, ref, watch } from 'vue';
import { RouterView, useRoute } from 'vue-router';
import { StatusPill, ThemeSwitcher, UiIcon, NyabulaLogo } from '@nyabula/ui';
import { useShell } from '../../../composables/useShell';
import ConnectionBanner from '../../../components/ConnectionBanner.vue';
import PanelUpdateBanner from '../../../components/PanelUpdateBanner.vue';

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
const showBottom = computed(() => shell.depth.value === 0);
const idx = (i: number) => String(i + 1).padStart(2, '0');
</script>

<template>
  <div class="ins-phone">
    <header class="bar">
      <button v-if="shell.depth.value > 0" class="bar-key" aria-label="返回" @click="shell.back()"><UiIcon name="arrow_back" :size="20" /></button>
      <NyabulaLogo v-else :size="24" class="bar-logo" />
      <h1 class="bar-title">{{ shell.title.value }}</h1>
      <StatusPill :tone="shell.connTone.value" :label="shell.connLabel.value" class="bar-pill" />
      <ThemeSwitcher compact />
    </header>
    <ConnectionBanner />
      <PanelUpdateBanner />
    <main class="main" :class="{ 'has-bottom': showBottom }">
      <RouterView v-slot="{ Component }">
        <Transition :name="transitionName">
          <component :is="Component" :key="route.path" class="stack-page" />
        </Transition>
      </RouterView>
    </main>
    <nav v-show="showBottom" class="bottom" aria-label="主导航">
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
.ins-phone {
  display: flex;
  flex-direction: column;
  height: 100dvh;
  background-color: var(--md-surface);
  background-image:
    linear-gradient(var(--ins-grid) 1px, transparent 1px),
    linear-gradient(90deg, var(--ins-grid) 1px, transparent 1px);
  background-size: 24px 24px;
}
.bar {
  display: flex;
  align-items: center;
  gap: 8px;
  height: calc(52px + var(--safe-t));
  padding: var(--safe-t) 12px 0;
  flex: none;
  border-bottom: 1px solid var(--md-outline-variant);
  background: var(--md-surface-container);
}
.bar-key {
  width: 36px;
  height: 36px;
  display: grid;
  place-items: center;
  border: none;
  border-radius: 3px;
  background: transparent;
  color: var(--md-on-surface);
  box-shadow: inset 0 0 0 1px var(--md-outline-variant);
}
.bar-key:active { transform: translateY(1px); }
.bar-logo { margin: 0 4px 0 2px; }
.bar-title { font: 400 17px var(--font-title); color: var(--md-on-surface); margin: 0; flex: 1; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.bar-pill { max-width: 40vw; overflow: hidden; }
.main { flex: 1; min-height: 0; overflow-y: auto; position: relative; }
.stack-page { min-height: 100%; }
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
  gap: 1px;
  padding: 0;
  border: none;
  border-right: 1px solid var(--md-outline-variant);
  background: transparent;
  color: var(--md-on-surface-variant);
  font: 600 10.5px var(--font-body);
  cursor: pointer;
}
.bkey:last-child { border-right: none; }
.bkey:active { transform: translateY(1px); }
.bkey.disabled { opacity: 0.38; }
.bkey.active { color: var(--md-on-surface); background: var(--md-surface); }
.bkey-top { display: flex; align-items: center; gap: 4px; }
.led { width: 5px; height: 5px; border-radius: 50%; background: var(--md-outline); }
.active .led { background: var(--md-primary); box-shadow: var(--ins-led-glow); }
.k-idx { font: 600 9.5px var(--ins-mono); letter-spacing: 0.08em; color: var(--md-outline); }
</style>
