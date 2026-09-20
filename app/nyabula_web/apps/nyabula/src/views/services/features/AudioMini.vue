<script setup lang="ts">
import { computed } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';
import { useNativeMedia } from '../../../composables/useNativeMedia';
import type { FeatureMiniProps } from './contract';
defineProps<FeatureMiniProps>();
const media = useNativeMedia();
const label = computed(() => !media.available ? '播放服务未提供' : media.state?.volumeSupported ? `${media.state.muted ? '静音' : media.state.volume + '%'} · ${media.state.device}` : '驱动未提供音量调整');
function toggle(): void { if (media.state) void media.action('volume', { volume: media.state.volume, muted: !media.state.muted }); }
</script>
<template><div class="native-audio-mini"><UiIcon name="volume_up" :size="20" /><span>{{ label }}</span><MdButton variant="text" :disabled="!media.available || media.busy || !media.state?.volumeSupported" @click="toggle">{{ media.state?.muted ? '取消静音' : '静音' }}</MdButton></div></template>
<style scoped>.native-audio-mini { display: flex; align-items: center; gap: 8px; min-height: 44px; }.native-audio-mini span { flex: 1; min-width: 0; font-size: 13px; overflow-wrap: anywhere; }</style>
