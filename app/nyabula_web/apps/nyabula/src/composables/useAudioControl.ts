/* The ES8388 codec service (`audio.*`): one shared store for the feature page
 * and its card, polled every 3 s only while a component that uses it is
 * mounted and the tab is visible.
 *
 * `supported` is three-valued: null until the device has answered once, false
 * when it answered ENOTFOUND (a firmware without the service; callers then
 * keep using music.volume), true otherwise. It goes back to null with every
 * new connection, which may land on a different firmware. */
import { computed, ref, watch } from 'vue';
import { defineStore } from 'pinia';
import { describeError } from '@nyabula/ui';
import { useSessionStore } from '../stores/session';
import { useVisiblePoll } from './usePageVisible';
import { isAudioUnsupported, parseAudioStatus, type AudioStatus } from '../lib/audioControl';

export type AudioSetter = 'output.route' | 'volume' | 'input.route' | 'mic.gain' | 'mic.mute' | 'channel';

const POLL_MS = 3000;

const useAudioControlStore = defineStore('audio-control', () => {
  const session = useSessionStore();
  const status = ref<AudioStatus | null>(null);
  const supported = ref<boolean | null>(null);
  const busy = ref(false);
  const error = ref('');
  /** The codec is there and this role may change it. */
  const canWrite = computed(() => session.canControl && supported.value === true && status.value?.available === true);
  let generation = 0;
  let refreshing = false;
  function reset(): void { generation++; status.value = null; supported.value = null; busy.value = false; refreshing = false; error.value = ''; }
  watch(() => session.client, reset);

  function accept(raw: unknown): boolean {
    const parsed = parseAudioStatus(raw);
    supported.value = true;
    if (!parsed) { error.value = '设备返回的音频状态无法识别'; return false; }
    status.value = parsed;
    return true;
  }
  function fail(cause: unknown): void {
    if (isAudioUnsupported(cause)) { supported.value = false; status.value = null; error.value = ''; }
    else error.value = describeError(cause);
  }
  async function refresh(): Promise<void> {
    if (!session.connected || refreshing || busy.value || supported.value === false) return;
    const current = generation;
    refreshing = true;
    try {
      const data = await session.request('audio.status', {});
      if (current === generation && accept(data)) error.value = '';
    } catch (cause) { if (current === generation) fail(cause); }
    finally { if (current === generation) refreshing = false; }
  }
  /** Every setter answers with the status that results from it. */
  async function set(setter: AudioSetter, data: Record<string, unknown>): Promise<boolean> {
    if (!canWrite.value || busy.value) return false;
    const current = generation;
    busy.value = true;
    try {
      const result = await session.request(`audio.${setter}`, data);
      if (current !== generation) return false;
      const ok = accept(result);
      if (ok) error.value = '';
      return ok;
    } catch (cause) { if (current === generation) fail(cause); return false; }
    finally { if (current === generation) busy.value = false; }
  }
  return { status, supported, busy, error, canWrite, refresh, set };
});

export function useAudioControl() {
  const audio = useAudioControlStore();
  const session = useSessionStore();
  watch(() => session.connected, ready => { if (ready) void audio.refresh(); }, { immediate: true });
  useVisiblePoll(() => { void audio.refresh(); }, POLL_MS);
  return audio;
}
