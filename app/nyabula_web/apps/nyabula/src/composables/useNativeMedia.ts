import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { defineStore } from 'pinia';
import { useSessionStore } from '../stores/session';

export interface NativeMediaState {
  state: 'idle' | 'playing' | 'paused'; track: string; device: string; root: string;
  volume: number; muted: boolean; volumeSupported: boolean; seekSupported: boolean;
  settingsSaved: boolean; busy: boolean; elapsedMs: number; durationMs: number;
  sampledAtMs: number; lastError: number;
}
export interface NativeTrack { name: string; supported: boolean; durationMs?: number; sampleRate?: number; channels?: number; bits?: number; error?: number }
export interface NativeLibrary { root: string; available: boolean; items: NativeTrack[]; error?: number }

const useNativeMediaStore = defineStore('native-media', () => {
  const session = useSessionStore();
  const state = ref<NativeMediaState | null>(null);
  const library = ref<NativeLibrary | null>(null);
  const busy = ref(false);
  const error = ref('');
  const available = computed(() => session.connected && session.isOwner && session.client?.capabilities.includes('core.media-v1') === true);
  let generation = 0;
  let refreshing = false;
  function reset(): void { generation++; state.value = null; library.value = null; busy.value = false; refreshing = false; error.value = ''; }
  watch(() => session.client, reset);
  watch(available, value => { if (!value) reset(); });
  function accept(data: Record<string, unknown>): void {
    const incoming = data as unknown as NativeMediaState;
    if (!state.value || incoming.sampledAtMs >= state.value.sampledAtMs) state.value = incoming;
  }
  async function refresh(): Promise<void> {
    if (!available.value || refreshing || busy.value) return;
    const current = generation;
    refreshing = true;
    try {
      const data = await session.request('music.status', {});
      /* A reading that arrives is also the answer to whatever failed before:
       * without this the banner of one refused play stayed on the page (and,
       * the store being shared, on the audio page too) until the next
       * successful button press.
       */
      if (current === generation) { accept(data); error.value = ''; }
    } catch (cause) { if (current === generation) error.value = String(cause); }
    finally { if (current === generation) refreshing = false; }
  }
  async function refreshLibrary(): Promise<void> {
    if (!available.value) return;
    const current = generation;
    try {
      const data = await session.request('music.library', {});
      if (current === generation) library.value = data as unknown as NativeLibrary;
    } catch (cause) { if (current === generation) error.value = String(cause); }
  }
  async function action(operation: string, data: Record<string, unknown> = {}): Promise<boolean> {
    if (!available.value || busy.value) return false;
    const current = generation;
    busy.value = true;
    try {
      const result = await session.request(`music.${operation}`, data);
      if (current !== generation) return false;
      accept(result); error.value = '';
      return true;
    } catch (cause) { if (current === generation) error.value = String(cause); return false; }
    finally { if (current === generation) { busy.value = false; void refresh(); } }
  }
  return { state, library, busy, error, available, refresh, refreshLibrary, action };
});

export function useNativeMedia() {
  const media = useNativeMediaStore();
  watch(() => media.available, ready => { if (ready) { void media.refresh(); void media.refreshLibrary(); } }, { immediate: true });
  const timer = setInterval(() => { void media.refresh(); }, 750);
  onBeforeUnmount(() => clearInterval(timer));
  return media;
}
