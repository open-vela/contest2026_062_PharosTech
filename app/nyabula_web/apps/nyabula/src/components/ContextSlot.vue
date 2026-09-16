<script setup lang="ts">
/* Page-side portal into the shell's context panel. Desktop shell mounts a
 * `#shell-context` target; other shells do not, in which case the content is
 * rendered inline (fallback) so nothing is lost. */
import { inject, onBeforeUnmount, onMounted, ref } from 'vue';
import { contextPresence } from '../composables/contextPresence';

const hasTarget = ref(false);
const shellHasContext = inject<boolean>('shellHasContext', false);
onMounted(() => {
  hasTarget.value = shellHasContext && !!document.getElementById('shell-context');
  if (hasTarget.value) contextPresence.count++;
});
onBeforeUnmount(() => {
  if (hasTarget.value) contextPresence.count--;
});
</script>

<template>
  <Teleport v-if="hasTarget" to="#shell-context"><slot /></Teleport>
  <div v-else class="context-inline"><slot /></div>
</template>

<style scoped>
.context-inline { display: contents; }
</style>
