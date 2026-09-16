<script setup lang="ts">
/* Core owns audio playback. Lyrics here remain explicit display drafts. */
import { computed, ref, watch } from 'vue';
import { MdButton, MdTextField, NkListSection, NkMediaPlayer, NkRow, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useNativeMedia } from '../../../composables/useNativeMedia';
import { useDeviceRuntime } from '../../../composables/useDeviceRuntime';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';
const props = defineProps<{ type: string; ff: FormFactor }>();
const media = useNativeMedia();
const device = useDeviceRuntime();
const eye = useEyeStore();
const selected = ref('');
const switching = ref(false);
const tracks = computed(() => media.library?.items.filter(item => item.supported) ?? []);
const outputReady = computed(() => device.snapshot?.audioDevices.some(item => item.path === media.state?.device && item.available && item.output) === true);
const mem = useFeatureMemory(props.type, { prev: '', current: '', next: '' });
const lyricPrev = ref(mem.prev), lyricCurrent = ref(mem.current), lyricNext = ref(mem.next);
const lyrics = computed(() => ({ prev: lyricPrev.value, current: lyricCurrent.value, next: lyricNext.value }));
watch(lyrics, value => saveFeatureMemory(props.type, value));
watch([tracks, () => media.state?.track], () => {
  if (media.state?.track && tracks.value.some(item => item.name === media.state?.track)) selected.value = media.state.track;
  else if (!tracks.value.some(item => item.name === selected.value)) selected.value = tracks.value[0]?.name ?? '';
}, { immediate: true });
const playing = computed(() => media.state?.state === 'playing');
const shown = computed(() => eye.activeScene === props.type);
const disabled = computed(() => !media.available || media.busy || switching.value || !selected.value || !outputReady.value);
const label = computed(() => !media.available ? '当前设备未启用原生播放服务' : !outputReady.value ? '未找到可用输出设备' : media.state?.state === 'playing' ? '设备正在播放' : media.state?.state === 'paused' ? '设备已暂停' : '设备未播放');
async function play(): Promise<void> {
  if (media.state?.state === 'paused') await media.action('resume');
  else await media.action('play', { name: selected.value });
}
async function select(name: string, start = false): Promise<void> {
  if (switching.value || media.busy) return;
  switching.value = true;
  try {
    const wasPlaying = !!media.state && media.state.state !== 'idle';
    if (wasPlaying && !await media.action('stop')) return;
    selected.value = name;
    if (wasPlaying || start) await media.action('play', { name });
  } finally { switching.value = false; }
}
function adjacent(delta: number): void {
  if (!tracks.value.length) return;
  const current = tracks.value.findIndex(item => item.name === selected.value);
  const next = tracks.value[(current + delta + tracks.value.length) % tracks.value.length];
  if (next) void select(next.name, true);
}
function push(): void {
  const state = media.state;
  if (!state) return;
  void eye.setScene(props.type, eye.sceneStyle, { title: state.track || selected.value,
    artist: '本地文件', playing: state.state === 'playing', position_ms: state.elapsedMs,
    duration_ms: state.durationMs, lyrics: lyrics.value });
}
function format(ms = 0): string { const seconds = Math.floor(ms / 1000); return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`; }
</script>
<template>
  <div class="feature native-music" :class="ff">
    <p role="status">{{ label }}</p>
    <p v-if="device.snapshot?.simulator" class="muted">当前输出为模拟器音频设备，不代表 K7 扬声器。</p>
    <p v-if="media.error" class="error" role="alert">{{ media.error }}</p>
    <div class="grid">
      <section class="col">
        <NkMediaPlayer :title="selected || '媒体目录中没有可播放曲目'" artist="设备本地文件"
          :playing="playing" :position="(media.state?.elapsedMs ?? 0) / 1000" :duration="(media.state?.durationMs ?? 0) / 1000"
          :seekable="false" :volume="media.state?.volume ?? 40" :show-volume="media.state?.volumeSupported ?? false"
          :lyrics="lyrics" :disabled="disabled" @play="play" @pause="media.action('pause')"
          @prev="adjacent(-1)" @next="adjacent(1)" @volume="level => media.action('volume', { volume: level, muted: media.state?.muted ?? false })" />
        <div class="actions">
          <MdButton variant="outlined" :disabled="!media.available || media.busy || !media.state || media.state.state === 'idle'" @click="media.action('stop')">停止播放</MdButton>
          <MdButton variant="text" :disabled="!media.available || media.busy" @click="media.refreshLibrary()">刷新本地曲目</MdButton>
        </div>
        <p class="hint muted">进度为设备播放计时，暂不支持跳转。关闭页面或猫眼展示不会停止音频。</p>
        <p v-if="!media.state?.volumeSupported" class="hint muted">当前输出驱动未提供音量调整。</p>
      </section>
      <section class="col">
        <NkListSection title="本地曲目">
          <p class="muted path">{{ media.library?.root ?? '等待设备目录' }}</p>
          <NkRow v-for="track in tracks" :key="track.name" icon="music_note" :title="track.name" :sub="`${format(track.durationMs)} · ${track.sampleRate} Hz · ${track.channels} 声道`" :tappable="media.available && !media.busy && !switching" @tap="select(track.name)">
            <UiIcon v-if="selected === track.name" name="check" :size="20" />
          </NkRow>
          <p v-if="!tracks.length" class="muted">请放入 16-bit PCM WAV 文件；暂不解码压缩格式或带尾随数据块的 WAV。</p>
          <p v-if="media.library?.items.some(item => !item.supported)" class="muted">目录中有当前不支持的文件，未列为可播放曲目。</p>
        </NkListSection>
        <NkListSection title="猫眼歌词预览">
          <div class="lyrics">
            <MdTextField v-model="lyricPrev" label="上一句" />
            <MdTextField v-model="lyricCurrent" label="当前句" />
            <MdTextField v-model="lyricNext" label="下一句" />
            <div class="actions"><MdButton :disabled="!media.state" @click="push">显示当前快照</MdButton><MdButton variant="text" :disabled="!shown" @click="eye.setScene(null)">隐藏展示</MdButton></div>
            <p class="hint muted">这里只发送当前状态与手动歌词快照；实时频谱、歌词同步尚未接入。</p>
          </div>
        </NkListSection>
      </section>
    </div>
  </div>
</template>
<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.grid { display: grid; grid-template-columns: 1fr; gap: 16px; align-items: start; }
.feature.desktop .grid { grid-template-columns: 1.1fr 1fr; }
.col { display: flex; flex-direction: column; gap: 14px; min-width: 0; }
.actions { display: flex; gap: 8px; flex-wrap: wrap; }
.hint { font-size: 12.5px; margin: 0; }
.lyrics { display: flex; flex-direction: column; gap: 10px; padding: 8px; }
.path { overflow-wrap: anywhere; }
.error { color: var(--md-error); }
</style>
