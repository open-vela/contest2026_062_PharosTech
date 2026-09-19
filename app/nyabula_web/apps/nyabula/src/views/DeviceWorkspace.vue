<script setup lang="ts">
/* Device workspace wrapper: owns the pairing overlay lifecycle and loads the
 * plugin list once connected so every child page has it. */
import { watch } from 'vue';
import { RouterView } from 'vue-router';
import { useSessionStore } from '../stores/session';
import { usePluginsStore } from '../stores/plugins';

defineProps<{ key: string }>();
const session = useSessionStore();
const plugins = usePluginsStore();
watch(
  () => session.connected,
  (c) => {
    if (c) void plugins.refresh();
  },
  { immediate: true },
);
</script>

<template>
  <RouterView />
</template>
