<script setup lang="ts">
/* The card says where sound comes out and mutes it: through the codec service
 * when the firmware has one, through the player's volume otherwise. */
import { computed } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';
import { useNativeMedia } from '../../../composables/useNativeMedia';
import { useAudioControl } from '../../../composables/useAudioControl';
import { audioSummary } from '../../../lib/audioControl';
import { useAudioScene } from './sceneLinks';
import type { FeatureMiniProps } from './contract';
import EyeShowButton from './EyeShowButton.vue';
const props = defineProps<FeatureMiniProps>();
const media = useNativeMedia();
const audio = useAudioControl();
const scene = useAudioScene(props.type, media, audio);
const codec = computed(() => (audio.supported === true && audio.status?.available ? audio.status : null));
const isMuted = computed(() => (codec.value ? codec.value.output.muted : media.state?.muted === true));
const label = computed(() => {
  if (codec.value) return audioSummary(codec.value);
  if (audio.supported === true && audio.status) return audioSummary(audio.status);
  if (!media.available) return '播放服务未提供';
  return media.state?.volumeSupported ? `${media.state.muted ? '静音' : media.state.volume + '%'} · ${media.state.device}` : '驱动未提供音量调整';
});
const canToggle = computed(() => (codec.value ? audio.canWrite && !audio.busy : media.available && !media.busy && media.state?.volumeSupported === true));
function toggle(): void {
  if (codec.value) void audio.set('volume', { muted: !codec.value.output.muted });
  else if (media.state) void media.action('volume', { volume: media.state.volume, muted: !media.state.muted });
}
</script>
<template><div class="native-audio-mini"><UiIcon :name="isMuted ? 'volume_off' : 'volume_up'" :size="20" /><span>{{ label }}</span><MdButton variant="text" :disabled="!canToggle" @click="toggle">{{ isMuted ? '取消静音' : '静音' }}</MdButton><EyeShowButton kind="round" :shown="active || scene.held.value" :disabled="!scene.canShow.value" @toggle="scene.toggle()" /></div></template>
<style scoped>.native-audio-mini { display: flex; align-items: center; gap: 8px; min-height: 44px; }.native-audio-mini span { flex: 1; min-width: 0; font-size: 13px; overflow-wrap: anywhere; }</style>
