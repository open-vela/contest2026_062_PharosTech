import { computed, ref, shallowRef, watch } from 'vue';
import { defineStore } from 'pinia';
import { describeError } from '@nyabula/ui';
import { useSessionStore } from './session';
import { isPrivacyState, type PrivacyState } from '../lib/privacy';

export const usePrivacyStore = defineStore('privacy', () => {
  const session = useSessionStore();
  const state = shallowRef<PrivacyState>();
  const busy = ref(false), error = ref(''), readError = ref('');
  const supported = computed(() => session.client?.capabilities.includes('core.privacy-v1') === true);
  const canWrite = computed(() => session.connected && session.isOwner && supported.value && !!state.value && !busy.value);
  const label = computed(() => !session.connected ? '设备离线' : !supported.value ? '设备固件尚未提供隐私开关' : !session.isOwner ? '仅主人可查看和设置' : !state.value ? '正在读取设备设置…' : '设置保存在设备，重启后仍生效');
  let generation = 0, reading = false;
  async function refresh() {
    if (!session.connected || !session.isOwner || !supported.value || reading || busy.value) return;
    reading = true; const version = generation, client = session.client;
    try {
      const value = await session.request('privacy.status');
      if (!isPrivacyState(value)) throw Error('设备返回了无效的隐私状态');
      if (version === generation && client === session.client) { state.value = value; readError.value = ''; }
    } catch (e) { if (version === generation && client === session.client) readError.value = describeError(e); }
    finally { reading = false; }
  }
  async function set(key: 'camera' | 'mic', enabled: boolean) {
    if (!canWrite.value || !state.value) return;
    const client = session.client, version = ++generation;
    busy.value = true; error.value = '';
    try {
      const value = await session.request('privacy.set', { [key]: enabled, revision: state.value.revision });
      if (version !== generation || client !== session.client) return;
      if (!isPrivacyState(value)) throw Error('设备未确认隐私设置');
      state.value = value;
    } catch (e) { if (version === generation && client === session.client) error.value = describeError(e); }
    finally { if (version === generation) { busy.value = false; void refresh(); } }
  }
  watch(() => [session.client, session.connected, session.isOwner, supported.value], () => {
    generation++; state.value = undefined; busy.value = false; error.value = ''; readError.value = ''; void refresh();
  }, { immediate: true });
  return { state, busy, error, readError, supported, canWrite, label, refresh, set };
});
