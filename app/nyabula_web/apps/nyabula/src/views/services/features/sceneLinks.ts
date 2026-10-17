/* Eye links shared by a feature page and its card: each turns the device data
 * the feature already reads into the scene payload (composables/eyeScenePayload)
 * and keeps it fresh while shown (composables/useEyeScene). No link asks the
 * device for anything by itself. */
import { useEyeScene } from '../../../composables/useEyeScene';
import {
  audioScene, briefingScene, companionScene, devicesScene, musicScene, networkScene, systemScene, weatherScene,
} from '../../../composables/eyeScenePayload';
import { useBriefingStore } from '../../../stores/briefing';
import { useCompanionStore } from '../../../stores/companion';
import { useWeatherStore, weatherKind } from '../../../stores/weather';
import type { useNativeMedia } from '../../../composables/useNativeMedia';
import type { useDeviceRuntime } from '../../../composables/useDeviceRuntime';
import type { useAudioControl } from '../../../composables/useAudioControl';
import { activeOutputLabel, sceneAudioRoute } from '../../../lib/audioControl';

type Media = ReturnType<typeof useNativeMedia>;
type Device = ReturnType<typeof useDeviceRuntime>;
type Audio = ReturnType<typeof useAudioControl>;

const BRIEFING_SOURCE: Record<string, string> = {
  QWeather: '和风天气', calendar: '设备日程', tasks: '设备待办', memory: '保存的记忆', device: '设备',
};
export const briefingSource = (value: string): string => BRIEFING_SOURCE[value] ?? value;

export function useBriefingScene(type: string) {
  const briefing = useBriefingStore();
  return useEyeScene(type, () => {
    const state = briefing.state;
    const item = state?.items[state.index] ?? state?.items[0];
    if (!state || !item) return null;
    return briefingScene({ index: state.items.indexOf(item), count: state.items.length, title: item.title, source: briefingSource(item.source), playing: state.playing });
  }, { hideWhenEmpty: true });
}

export function useCompanionScene(type: string) {
  const companion = useCompanionStore();
  return useEyeScene(type, () => {
    const s = companion.state;
    return s ? companionScene({ enabled: s.enabled, mode: s.mode, quietNow: s.quiet_now, dailyCount: s.daily_count, dailyLimit: s.daily_limit }) : null;
  });
}

export function useDevicesScene(type: string, device: Device) {
  return useEyeScene(type, () => (device.snapshot ? devicesScene(device.snapshot) : null));
}

export function useMusicScene(type: string, media: Media, lyrics?: () => { prev: string; current: string; next: string }) {
  return useEyeScene(type, () => {
    const s = media.state;
    if (!s) return null;
    return musicScene({ track: s.track, playing: s.state === 'playing', elapsedMs: s.elapsedMs, durationMs: s.durationMs, lyrics: lyrics?.() });
  });
}

/** The codec service says where sound really comes out; a firmware without it only knows the player's volume. */
export function useAudioScene(type: string, media: Media, audio?: Audio) {
  return useEyeScene(type, () => {
    const live = audio?.status;
    if (live?.available) {
      return audioScene({ volume: live.output.volume, muted: live.output.muted, device: '', route: sceneAudioRoute(live.output), title: activeOutputLabel(live.output) });
    }
    const s = media.state;
    return s ? audioScene({ volume: s.volume, muted: s.muted, device: s.device }) : null;
  });
}

export function useNetworkScene(type: string, read: () => { ssid: string | null; rssi: number | null; connected: boolean } | null) {
  return useEyeScene(type, () => { const n = read(); return n ? networkScene(n) : null; });
}

export function useSystemScene(type: string, read: () => { cpuPercent: number | null; memoryPercent: number | null; uptimeText: string; version: string } | null) {
  return useEyeScene(type, () => { const s = read(); return s ? systemScene(s) : null; }, { intervalMs: 5000 });
}

const KPH: Record<string, number> = { 'km/h': 1, 'm/s': 3.6, mph: 1.609344 };
export function useWeatherScene(type: string) {
  const weather = useWeatherStore();
  return useEyeScene(type, () => {
    const now = weather.now;
    if (!now.temperature) return null;
    const wind = now.wind?.speed;
    return weatherScene({
      kind: weatherKind(now.condition?.code), city: now.location?.name, temperature: now.temperature.value,
      feelsLike: now.feelsLike?.value, humidity: now.humidity,
      windKph: wind ? wind.value * (KPH[wind.unit] ?? 1) : undefined,
      visibilityKm: now.visibility ? now.visibility.value * (now.visibility.unit === 'm' ? 0.001 : 1) : undefined,
    });
  }, { intervalMs: 5000 });
}
