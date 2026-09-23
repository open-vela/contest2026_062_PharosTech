<script setup lang="ts">
/* Root: picks the shell for the current form factor and hosts the global
 * feedback layers (boot overlay, route curtain, modal, toasts).
 * `meta.bare` routes (device build: boot, login, password setup, WiFi
 * provisioning) get no shell at all on any form factor. */
import { computed, provide, watch } from 'vue';
import { useRoute } from 'vue-router';
import { AppLoading, AppModal, RouteLoading, ToastHost, useDialogStore, useLoadingStore } from '@nyabula/ui';
import { useFormFactor } from './composables/useFormFactor';
import DesktopShell from './layouts/DesktopShell.vue';
import TabletShell from './layouts/TabletShell.vue';
import PhoneShell from './layouts/PhoneShell.vue';
import BareShell from './layouts/BareShell.vue';
import { useDeviceAccessGuard } from './composables/useDeviceAccessGuard';
import NotificationCenter from './components/NotificationCenter.vue';

const ff = useFormFactor();
provide('formFactor', ff);

const route = useRoute();
/* Device build: also bare before the first route resolves, so the app shell never flashes. */
const bare = computed(() => route.meta.bare === true || (__NYA_DEVICE__ && route.matched.length === 0));
const shell = computed(() => (bare.value ? BareShell : ff.formFactor.value === 'desktop' ? DesktopShell : ff.formFactor.value === 'tablet' ? TabletShell : PhoneShell));
if (__NYA_DEVICE__) useDeviceAccessGuard();
const dialog = useDialogStore();
const loading = useLoadingStore();
/* Route curtain: the whole shell shrinks 10% behind it (Myself behaviour);
 * while any overlay is on screen, CSS animations underneath pause. */
const shrunk = computed(() => loading.routeLoading);
const covered = computed(() => loading.bootOverlayVisible || loading.routeOverlayVisible);
watch(
  () => ff.formFactor.value,
  (v) => {
    dialog.sheetMode = v === 'phone';
    document.documentElement.dataset.ff = v;
  },
  { immediate: true },
);
</script>

<template>
  <div class="app-shell" :class="{ shrunk, covered }">
    <component :is="shell" />
  </div>
  <ToastHost :bottom="bare ? 'calc(24px + var(--safe-b))' : ff.isPhone.value ? 'calc(var(--shell-bottom) + 12px + var(--safe-b))' : '24px'" />
  <AppModal />
  <NotificationCenter v-if="!bare" />
  <RouteLoading />
  <AppLoading />
</template>

<style scoped>
.app-shell {
  height: 100dvh;
  overflow: hidden;
  transform-origin: 50% 50%;
  transition: transform 0.55s var(--ease-out);
}
.app-shell.shrunk {
  transform: scale(0.9);
}
.app-shell.covered :deep(*) {
  animation-play-state: paused !important;
}
</style>
