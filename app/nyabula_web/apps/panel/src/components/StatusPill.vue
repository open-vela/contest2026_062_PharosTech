<script setup lang="ts">
/* Thin wrapper: maps the panel link store state onto the shared pill. */
import { computed } from 'vue';
import { StatusPill } from '@nyabula/ui';
import { useLinkStore } from '../stores/link';

const link = useLinkStore();

const label = computed(() => {
  switch (link.state) {
    case 'connected':
      return link.device?.name ? `已连接 · ${link.device.name}` : '已连接';
    case 'connecting':
      return '连接中…';
    case 'authenticating':
      return '鉴权中…';
    case 'pairing-required':
      return '待配对';
    case 'reconnecting':
      return '重连中…';
    case 'closed':
      return '已断开';
    default:
      return '未连接';
  }
});
const tone = computed(() =>
  link.state === 'connected' ? 'ok' : link.state === 'reconnecting' || link.state === 'connecting' || link.state === 'authenticating' ? 'busy' : 'off',
);
</script>

<template>
  <StatusPill :tone="tone" :label="label" />
</template>
