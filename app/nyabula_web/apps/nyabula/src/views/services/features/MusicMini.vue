<script setup lang="ts">
import { computed, ref } from 'vue';
import { UiIcon } from '@nyabula/ui';
import { useNativeMedia } from '../../../composables/useNativeMedia';
import { useDeviceRuntime } from '../../../composables/useDeviceRuntime';
import type { FeatureMiniProps } from './contract';
defineProps<FeatureMiniProps>();
const media = useNativeMedia();
const device = useDeviceRuntime();
const switching = ref(false);
const tracks = computed(() => media.library?.items.filter(item => item.supported) ?? []);
const title = computed(() => media.state?.track || tracks.value[0]?.name || '没有本地曲目');
const playing = computed(() => media.state?.state === 'playing');
const ready = computed(() => media.available && !media.busy && !switching.value && !!tracks.value.length && device.snapshot?.audioDevices.some(item => item.available && item.output && item.path === media.state?.device));
const progress = computed(() => media.state?.durationMs ? Math.min(100, media.state.elapsedMs / media.state.durationMs * 100) : 0);
async function toggle(): Promise<void> {
  if (!ready.value) return;
  if (playing.value) await media.action('pause');
  else if (media.state?.state === 'paused') await media.action('resume');
  else await media.action('play', { name: media.state?.track || tracks.value[0]?.name });
}
async function move(delta: number): Promise<void> {
  if (!ready.value) return;
  switching.value = true;
  try {
    const index = tracks.value.findIndex(item => item.name === media.state?.track);
    const next = tracks.value[(Math.max(index, 0) + delta + tracks.value.length) % tracks.value.length];
    if (next && await media.action('stop')) await media.action('play', { name: next.name });
  } finally { switching.value = false; }
}
</script>
<template>
  <div class="mm native-music-mini">
    <div class="row"><div class="meta"><strong>{{ title }}</strong><span>{{ !media.available ? '播放服务未提供' : playing ? '设备播放中' : media.state?.state === 'paused' ? '设备已暂停' : '设备未播放' }}</span></div>
      <button type="button" aria-label="上一首" :disabled="!ready" @click="move(-1)"><UiIcon name="skip_previous" :size="20" /></button>
      <button type="button" :aria-label="playing ? '暂停' : '播放'" :disabled="!ready" @click="toggle"><UiIcon :name="playing ? 'pause' : 'play_arrow'" :size="22" /></button>
      <button type="button" aria-label="下一首" :disabled="!ready" @click="move(1)"><UiIcon name="skip_next" :size="20" /></button>
    </div><div class="bar" role="progressbar" :aria-valuenow="Math.round(progress)" aria-valuemin="0" aria-valuemax="100"><span :style="{ width: `${progress}%` }" /></div>
  </div>
</template>
<style scoped>
.mm { display: flex; flex-direction: column; gap: 8px; }
.row { display: flex; align-items: center; gap: 4px; }
.meta { flex: 1; min-width: 0; display: flex; flex-direction: column; }
.meta strong { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; font-size: 14px; }
.meta span { font-size: 12px; color: var(--md-on-surface-variant); }
button { width: 44px; height: 44px; border: 0; border-radius: 50%; background: var(--md-surface-container-high); color: var(--md-on-surface); display: grid; place-items: center; cursor: pointer; flex: none; }
button:disabled { opacity: .4; cursor: default; }
.bar { height: 4px; background: var(--md-surface-container-high); border-radius: 2px; overflow: hidden; }
.bar span { display: block; height: 100%; background: var(--md-primary); }
</style>
