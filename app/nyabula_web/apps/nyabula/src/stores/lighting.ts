import { computed, ref, shallowRef, watch } from 'vue';
import { defineStore } from 'pinia';
import { describeError } from '@nyabula/ui';
import { useSessionStore } from './session';
import { lightingStatus, type LightChange, type LightingStatus, type LightZone } from '../lib/lighting';

export const useLightingStore = defineStore('lighting', () => {
  const session = useSessionStore();
  const state = shallowRef<LightingStatus>();
  const busy = ref(false), error = ref(''), readError = ref('');
  const supported = computed(() => session.client?.capabilities.includes('core.lighting-v1') === true);
  const canWrite = computed(() => session.connected && session.canControl && supported.value && !!state.value?.available && !busy.value);
  const anyOn = computed(() => state.value?.groups.some(group => group.enabled) === true);
  const label = computed(() => !session.connected ? '设备离线' : !supported.value ? '设备固件尚未提供灯光控制' : !state.value ? '正在读取灯光…' : state.value.lastError ? '灯光输出异常' : !state.value.available ? '灯光设备未就绪' : state.value.phase === 'boot' ? '开机光效' : state.value.phase === 'sleeping' ? '渐暗入眠' : state.value.sleeping ? '休眠 · 灯光已熄灭' : anyOn.value ? `${state.value.groups.filter(g => g.enabled).length} 个区域开启` : '全部关闭');
  let generation = 0, reading = false;
  async function refresh() {
    if (!session.connected || !supported.value || reading || busy.value) return;
    reading = true; const version = generation, client = session.client;
    try {
      const value = await session.request('lighting.status');
      if (!lightingStatus(value)) throw Error('设备返回了无效的灯光状态');
      if (version === generation && client === session.client) { state.value = value; readError.value = ''; }
    } catch (e) { if (version === generation && client === session.client) readError.value = describeError(e); }
    finally { reading = false; }
  }
  async function action(topic: string, data: Record<string, unknown>) {
    if (!canWrite.value || !state.value) return false;
    const client = session.client, version = ++generation;
    busy.value = true; error.value = '';
    try {
      const value = await session.request(topic, { ...data, revision: state.value.revision });
      if (version !== generation || client !== session.client) return false;
      if (!lightingStatus(value)) throw Error('设备未返回有效的灯光设置');
      state.value = value; return true;
    } catch (e) { if (version === generation && client === session.client) error.value = describeError(e); return false; }
    finally { if (version === generation) { busy.value = false; void refresh(); } }
  }
  function set(zone: LightZone | 'all' | 'ears', change: LightChange) { return action('lighting.set', { group: zone, ...change }); }
  function preset(name: string) { return action('lighting.preset', { name }); }
  watch(() => [session.client, session.connected, supported.value], () => { generation++; state.value = undefined; busy.value = false; error.value = ''; readError.value = ''; void refresh(); }, { immediate: true });
  return { state, busy, error, readError, supported, canWrite, anyOn, label, refresh, set, preset };
});
