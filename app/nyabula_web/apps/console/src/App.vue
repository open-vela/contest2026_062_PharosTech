<script setup lang="ts">
/* Console root: the active skin's shell (login + console) plus the global
 * feedback layers (boot overlay, modal dialog, toasts). The skin comes from
 * the shared theme store, so it follows the device WebUI's choice. */
import { computed } from 'vue';
import { AppLoading, AppModal, ToastHost, useThemeStore } from '@nyabula/ui';
import { useAuthStore } from './stores/auth';
import DaylightShell from './skins/daylight/DaylightShell.vue';
import ClassicShell from './skins/classic/ClassicShell.vue';
import NebulaShell from './skins/nebula/NebulaShell.vue';
import InstrumentShell from './skins/instrument/InstrumentShell.vue';

const theme = useThemeStore();
const auth = useAuthStore();
const SHELLS = { daylight: DaylightShell, classic: ClassicShell, nebula: NebulaShell, instrument: InstrumentShell };
const shell = computed(() => SHELLS[theme.skin]);
</script>

<template>
  <component :is="shell" v-if="auth.ready" />
  <label v-if="auth.ready && !auth.user" class="guest-skin">界面风格
    <select :value="theme.skin" @change="theme.setSkin(($event.target as HTMLSelectElement).value as typeof theme.skin)">
      <option v-for="skin in theme.skins" :key="skin.id" :value="skin.id">{{ skin.name }}</option>
    </select>
  </label>
  <ToastHost bottom="24px" />
  <AppModal />
  <AppLoading />
</template>

<style scoped>
.guest-skin { position: fixed; left: 18px; top: 18px; z-index: 100; display: flex; gap: 8px; align-items: center; color: var(--md-on-surface-variant); font-size: 13px; background: var(--md-surface-container); padding: 8px 12px; border-radius: var(--radius-m); }
select { color: var(--md-on-surface); background: var(--md-surface-container-high); border: 1px solid var(--md-outline-variant); border-radius: 8px; padding: 5px; }
</style>
