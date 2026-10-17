import { computed, ref, watch } from 'vue';
import { defineStore } from 'pinia';
import { useSessionStore } from './session';

export interface WeatherQuantity { value: number; unit: string }
export interface WeatherCondition { text: string; code: string }
export interface WeatherLocation { name: string; adm1: string; adm2: string; country: string; tz: string }
export interface WeatherBase { fetched_at?: number; last_error?: number; location?: WeatherLocation; metadata?: { attributions?: string[]; zeroResult?: boolean } }
export interface WeatherCurrent extends WeatherBase {
  temperature?: WeatherQuantity; feelsLike?: WeatherQuantity; humidity?: number; condition?: WeatherCondition;
  wind?: { speed: WeatherQuantity; scale: number; direction: { compass: string } };
  pressure?: WeatherQuantity; visibility?: WeatherQuantity; uvIndex?: number;
}
export interface WeatherDay {
  forecastStartTime: string; temperatureMax: WeatherQuantity; temperatureMin: WeatherQuantity;
  daytime: { condition: WeatherCondition }; nighttime: { condition: WeatherCondition };
  astro?: { sunrise?: string; sunset?: string; moonPhase?: string };
}
export interface WeatherHour { forecastTime?: string; forecastStartTime?: string; temperature: WeatherQuantity; condition: WeatherCondition }
export interface WeatherAlert { id: string; headline: string; description: string; instruction?: string; senderName?: string; expireTime: string; messageType: { code: string; supersedes?: string[] } }
export interface WeatherStatus {
  revision: number; host: string; city: string; province: string; enabled: boolean; key_set: boolean;
  refreshing: boolean; last_error: number; clock_valid: boolean; errors?: Record<string, number>;
}

export const useWeatherStore = defineStore('weather', () => {
  const session = useSessionStore();
  const available = computed(() => session.connected && session.client?.capabilities.includes('core.weather-v1') === true);
  const status = ref<WeatherStatus | null>(null);
  const now = ref<WeatherCurrent>({});
  const daily = ref<WeatherBase & { days?: WeatherDay[] }>({});
  const hourly = ref<WeatherBase & { hours?: WeatherHour[] }>({});
  const alerts = ref<WeatherBase & { alerts?: WeatherAlert[] }>({});
  const error = ref('');
  const busy = ref(false);
  let generation = 0;
  let pending: Promise<void> | null = null;
  watch(() => [session.client, session.state], () => {
    generation++; pending = null; status.value = null; now.value = {}; daily.value = {};
    hourly.value = {}; alerts.value = {}; error.value = ''; busy.value = false;
  });
  function refresh(): Promise<void> {
    if (!available.value) return Promise.resolve();
    if (pending) return pending;
    const current = generation;
    pending = (async () => {
      try {
        const [s, n, d, h, a] = await Promise.all([
          session.request('weather.status'), ...['now', 'daily', 'hourly', 'alerts'].map(section => session.request('weather.get', { section })),
        ]);
        if (current !== generation) return;
        status.value = s as unknown as WeatherStatus;
        now.value = n as WeatherCurrent; daily.value = d; hourly.value = h; alerts.value = a;
        error.value = '';
      } catch (e) { if (current === generation) error.value = e instanceof Error ? e.message : String(e); }
      finally { if (current === generation) pending = null; }
    })();
    return pending;
  }
  async function configure(data: Record<string, unknown>): Promise<boolean> {
    if (!available.value || busy.value) return false;
    const current = generation;
    busy.value = true;
    try {
      const result = await session.request('weather.configure', data);
      if (current !== generation) return false;
      status.value = result as unknown as WeatherStatus;
      now.value = {}; daily.value = {}; hourly.value = {}; alerts.value = {};
      error.value = '';
      return true;
    } catch (e) { if (current === generation) error.value = e instanceof Error ? e.message : String(e); return false; }
    finally { if (current === generation) busy.value = false; }
  }
  async function fetchNow(): Promise<void> {
    if (!available.value || busy.value) return;
    const current = generation;
    try { await session.request('weather.refresh'); if (current === generation) await refresh(); }
    catch (e) { if (current === generation) error.value = e instanceof Error ? e.message : String(e); }
  }
  return { available, status, now, daily, hourly, alerts, error, busy, refresh, configure, fetchNow };
});

export function weatherKind(code?: string): 'sunny' | 'cloudy' | 'rain' | 'snow' | 'storm' | 'fog' {
  const value = Number(code);
  if (value === 100 || value === 150) return 'sunny';
  if (value === 302 || value === 303 || value === 304) return 'storm';
  if (value >= 300 && value < 400) return 'rain';
  if (value >= 400 && value < 500) return 'snow';
  if (value >= 500 && value < 600) return 'fog';
  return 'cloudy';
}
