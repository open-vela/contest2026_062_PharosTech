<script setup lang="ts">
/* Thin wrapper: binds the shared pure-render EyeCanvas to the panel link
 * store (eye.state in, eye.look out). Behavior identical to the previous
 * store-coupled component. */
import { EyeCanvas } from '@nyabula/ui';
import type { EyeEngine } from '@nyabula/eye-engine';
import { useLinkStore } from '../stores/link';

const link = useLinkStore();

const emit = defineEmits<{ (e: 'ready', engine: EyeEngine): void }>();

function onLook(data: { x?: number; y?: number; release?: boolean }) {
  void link.send('eye.look', data);
}
</script>

<template>
  <EyeCanvas
    :eye-state="link.lastEyeState"
    :clock-offset-ms="link.clockOffsetMs()"
    @ready="(e: EyeEngine) => emit('ready', e)"
    @look="onLook"
  />
</template>
