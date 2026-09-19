<script setup lang="ts">
/* Media player card: cover (placeholder when missing), title/artist,
 * seekable progress, prev/play-pause/next, volume slider and an optional
 * three-line lyrics area (prev/current/next with slide animation). */
import { computed, ref } from 'vue';
import MdSlider from '../components/MdSlider.vue';
import UiIcon from '../components/UiIcon.vue';

export interface NkLyricLines {
  prev?: string;
  current?: string;
  next?: string;
}
export interface NkMediaPlayerProps {
  title?: string;
  artist?: string;
  cover?: string;
  playing?: boolean;
  /** Seconds. */
  position?: number;
  /** Seconds; 0 hides the time labels. */
  duration?: number;
  /** 0..100 */
  volume?: number;
  showVolume?: boolean;
  seekable?: boolean;
  lyrics?: NkLyricLines;
  disabled?: boolean;
}
const props = withDefaults(defineProps<NkMediaPlayerProps>(), {
  title: '未在播放',
  artist: '',
  playing: false,
  position: 0,
  duration: 0,
  volume: 50,
  showVolume: true,
  seekable: true,
  disabled: false,
});
const emit = defineEmits<{
  (e: 'play'): void;
  (e: 'pause'): void;
  (e: 'prev'): void;
  (e: 'next'): void;
  (e: 'seek', seconds: number): void;
  (e: 'volume', level: number): void;
}>();

const seeking = ref(false);
const seekValue = ref(0);
const shownPos = computed(() => (seeking.value ? seekValue.value : props.position));
const fmt = (s: number): string => {
  const t = Math.max(0, Math.floor(s));
  return `${Math.floor(t / 60)}:${String(t % 60).padStart(2, '0')}`;
};
function onSeekInput(v: number): void {
  seeking.value = true;
  seekValue.value = v;
}
function onSeekChange(ev: Event): void {
  seeking.value = false;
  if (!props.disabled) emit('seek', Number((ev.target as HTMLInputElement).value));
}
function toggle(): void {
  if (props.disabled) return;
  if (props.playing) emit('pause');
  else emit('play');
}
const lyricKey = computed(() => props.lyrics?.current ?? '');
</script>

<template>
  <div class="nk-player" :class="{ disabled }">
    <div class="nk-player-top">
      <div class="nk-player-cover" :class="{ spinning: playing && !cover }">
        <img v-if="cover" :src="cover" alt="" />
        <UiIcon v-else name="music_note" :size="34" />
      </div>
      <div class="nk-player-meta">
        <div class="nk-player-title">{{ title }}</div>
        <div v-if="artist" class="nk-player-artist">{{ artist }}</div>
      </div>
    </div>

    <div v-if="lyrics" class="nk-player-lyrics" aria-live="polite">
      <Transition name="nk-lyric" mode="out-in">
        <div :key="lyricKey" class="nk-lyric-stack">
          <div class="nk-lyric prev">{{ lyrics.prev || ' ' }}</div>
          <div class="nk-lyric cur">{{ lyrics.current || ' ' }}</div>
          <div class="nk-lyric next">{{ lyrics.next || ' ' }}</div>
        </div>
      </Transition>
    </div>

    <div class="nk-player-seek">
      <MdSlider
        :model-value="shownPos"
        :min="0"
        :max="Math.max(1, duration)"
        :step="1"
        :disabled="disabled || duration <= 0 || !seekable"
        aria-label="进度"
        @update:model-value="onSeekInput"
        @change="onSeekChange"
      />
      <div v-if="duration > 0" class="nk-player-times">
        <span>{{ fmt(shownPos) }}</span>
        <span>{{ fmt(duration) }}</span>
      </div>
    </div>

    <div class="nk-player-controls">
      <button type="button" class="nk-player-btn" aria-label="上一首" :disabled="disabled" @click="emit('prev')">
        <UiIcon name="skip_previous" :size="28" />
      </button>
      <button type="button" class="nk-player-btn main" :aria-label="playing ? '暂停' : '播放'" :disabled="disabled" @click="toggle">
        <UiIcon :name="playing ? 'pause' : 'play_arrow'" :size="32" />
      </button>
      <button type="button" class="nk-player-btn" aria-label="下一首" :disabled="disabled" @click="emit('next')">
        <UiIcon name="skip_next" :size="28" />
      </button>
    </div>

    <div v-if="showVolume" class="nk-player-volume">
      <UiIcon :name="volume <= 0 ? 'volume_off' : volume < 50 ? 'volume_down' : 'volume_up'" :size="20" />
      <MdSlider
        :model-value="volume"
        :min="0"
        :max="100"
        :step="1"
        :disabled="disabled"
        aria-label="音量"
        @update:model-value="emit('volume', $event)"
      />
    </div>
  </div>
</template>

<style scoped>
.nk-player {
  display: flex; flex-direction: column; gap: 14px;
  padding: 16px;
  border-radius: var(--radius-l);
  background: var(--md-surface-container);
  color: var(--md-on-surface);
}
.nk-player.disabled { opacity: 0.5; }
.nk-player-top { display: flex; align-items: center; gap: 14px; }
.nk-player-cover {
  width: 72px; height: 72px; flex: none;
  border-radius: var(--radius-m); overflow: hidden;
  display: grid; place-items: center;
  background: var(--md-primary-container); color: var(--md-on-primary-container);
}
.nk-player-cover img { width: 100%; height: 100%; object-fit: cover; display: block; }
.nk-player-cover.spinning { border-radius: var(--radius-full); animation: nk-spin 6s linear infinite; }
@keyframes nk-spin { to { transform: rotate(360deg); } }
.nk-player-meta { flex: 1; min-width: 0; }
.nk-player-title { font: 600 17px var(--font-body); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.nk-player-artist { margin-top: 3px; font: 400 13px var(--font-body); color: var(--md-on-surface-variant); }
.nk-player-lyrics { height: 84px; overflow: hidden; text-align: center; }
.nk-lyric-stack { display: flex; flex-direction: column; gap: 6px; }
.nk-lyric { font: 500 13px var(--font-body); color: var(--md-on-surface-variant); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; min-height: 18px; }
.nk-lyric.cur { font: 600 16px var(--font-body); color: var(--md-primary); }
.nk-lyric-enter-active { transition: transform var(--dur) var(--ease-out), opacity var(--dur) var(--ease-out); }
.nk-lyric-leave-active { transition: transform var(--dur-fast) ease, opacity var(--dur-fast) ease; }
.nk-lyric-enter-from { transform: translateY(24px); opacity: 0; }
.nk-lyric-leave-to { transform: translateY(-24px); opacity: 0; }
.nk-player-seek :deep(.md-slider) { width: 100%; }
.nk-player-times { display: flex; justify-content: space-between; margin-top: 4px; font: 500 11px var(--font-body); color: var(--md-on-surface-variant); font-variant-numeric: tabular-nums; }
.nk-player-controls { display: flex; align-items: center; justify-content: center; gap: 18px; }
.nk-player-btn {
  width: 48px; height: 48px;
  border: none; border-radius: var(--radius-full);
  background: transparent; color: var(--md-on-surface);
  display: grid; place-items: center; cursor: pointer;
  transition: background var(--dur-fast), transform var(--dur-fast);
}
.nk-player-btn:hover { background: var(--md-surface-container-high); }
.nk-player-btn:active { transform: scale(0.94); }
.nk-player-btn.main { width: 64px; height: 64px; background: var(--md-primary); color: var(--md-on-primary); }
.nk-player-btn:focus-visible { outline: 2px solid var(--md-primary); outline-offset: 2px; }
.nk-player-btn:disabled { cursor: default; }
.nk-player-volume { display: flex; align-items: center; gap: 10px; min-height: 44px; color: var(--md-on-surface-variant); }
.nk-player-volume :deep(.md-slider) { flex: 1; }
</style>
