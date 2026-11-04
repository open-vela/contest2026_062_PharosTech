import { computed, onBeforeUnmount, ref, shallowRef, watch } from 'vue';
import { describeError } from '@nyabula/ui';
import { useSessionStore } from '../stores/session';
import { IntercomMicrophone, IntercomPlayback, encodeIntercom, isIntercomState, type IntercomState } from '../lib/intercomAudio';

export function useIntercom() {
  const session = useSessionStore();
  const state = shallowRef<IntercomState>();
  const active = ref(false), held = ref(false), pending = ref(false), releasing = ref(false), busy = ref(false);
  const error = ref('');
  const supported = computed(() => session.client?.capabilities.includes('core.intercom-v1') === true);
  const canStart = computed(() => session.connected && session.isOwner && supported.value && !busy.value);
  const microphoneSupported = typeof window !== 'undefined' && window.isSecureContext && !!navigator.mediaDevices?.getUserMedia;
  const label = computed(() => !session.connected ? '设备离线' : !supported.value ? '需要对讲版固件' : pending.value ? '正在连接浏览器麦克风' :
    held.value ? '正在向设备说话' : releasing.value ? '正在送出尾音' : active.value ? state.value?.phase === 'starting' ? '正在连接设备音频' : '正在收听设备' : '对讲未开启');
  let owner: typeof session.client = null;
  let playback: IntercomPlayback | null = null, microphone: IntercomMicrophone | null = null;
  let generation = 0, pressGeneration = 0, sequence = 0, stateVersion = 0;
  let talkReady = false, disposed = false;
  let queue = { chain: Promise.resolve() as Promise<void>, pending: 0 };
  let subscriptions: (() => void)[] = [];

  async function stop() {
    const client = owner; owner = null; generation++; pressGeneration++;
    active.value = held.value = pending.value = releasing.value = busy.value = false; talkReady = false;
    const mic = microphone, speaker = playback; microphone = null; playback = null;
    await Promise.allSettled([
      mic?.close(false), speaker?.close(),
      client?.state === 'connected' ? client.request('intercom.stop', {}) : Promise.resolve(),
    ]);
  }
  async function start() {
    if (!canStart.value || active.value) return;
    const client = session.client!, current = ++generation;
    busy.value = true; error.value = ''; state.value = undefined;
    queue = { chain: Promise.resolve(), pending: 0 };
    try {
      const speaker = await IntercomPlayback.open();
      if (current !== generation || disposed) { await speaker.close(); return; }
      playback = speaker; owner = client; active.value = true;
      const before = stateVersion;
      const value = await client.request('intercom.start', {});
      if (current !== generation || disposed) { await client.request('intercom.stop', {}); return; }
      if (!isIntercomState(value)) throw Error('设备返回了无效的对讲状态');
      if (before === stateVersion) state.value = value;
      if (value.phase === 'error') throw Error(`设备音频未就绪（${value.lastError}）`);
    } catch (e) { if (current === generation && !disposed) { error.value = describeError(e); await stop(); } }
    finally { if (current === generation && !disposed) busy.value = false; }
  }
  function send(bytes: Uint8Array, current: number) {
    const client = owner;
    if (!client || current !== generation || !active.value || !talkReady) return;
    const currentQueue = queue;
    if (currentQueue.pending >= 4) { error.value = '网络发送过慢，对讲已停止'; void stop(); return; }
    let data: string;
    try { data = encodeIntercom(bytes); }
    catch (e) { error.value = describeError(e); void stop(); return; }
    const number = sequence++; currentQueue.pending++;
    currentQueue.chain = currentQueue.chain.then(async () => {
      if (current !== generation || owner !== client) return;
      await client.request('intercom.audio', { sequence: number, data }, { timeoutMs: 3000 });
    }).catch(e => { if (current === generation && !disposed) { error.value = describeError(e); void stop(); } }).finally(() => { currentQueue.pending--; });
  }
  async function press() {
    if (!active.value || held.value || pending.value || releasing.value || !owner || !microphoneSupported) return;
    const client = owner, current = generation, press = ++pressGeneration;
    held.value = pending.value = true; error.value = ''; playback?.clear();
    sequence = 0; talkReady = false;
    try {
      const mic = await IntercomMicrophone.open(bytes => send(bytes, current));
      if (press !== pressGeneration || current !== generation || !held.value || disposed) { await mic.close(false); return; }
      microphone = mic;
      await client.request('intercom.talk.start', {});
      if (press !== pressGeneration || current !== generation || !held.value || disposed) return;
      talkReady = true; pending.value = false;
    } catch (e) { if (current === generation && !disposed) { error.value = describeError(e); await stop(); } }
  }
  async function release() {
    if (releasing.value) return;
    held.value = pending.value = false;
    const mic = microphone, client = owner, current = generation;
    microphone = null;
    if (!mic) { pressGeneration++; return; }
    releasing.value = true;
    try {
      await mic.close(true);
      await queue.chain;
      if (current === generation && client?.state === 'connected') await client.request('intercom.talk.stop', {});
    } catch (e) { if (current === generation && !disposed) { error.value = describeError(e); await stop(); } }
    finally { if (current === generation) { releasing.value = false; talkReady = false; pressGeneration++; } }
  }
  watch(() => [session.client, session.connected, session.role], () => {
    void stop(); state.value = undefined; error.value = '';
    subscriptions.forEach(off => off()); subscriptions = [];
    const client = session.client;
    if (!client) return;
    subscriptions.push(client.on('intercom.state', value => {
      if (owner !== client || disposed) return;
      if (!isIntercomState(value)) { error.value = '设备音频状态无效'; void stop(); return; }
      stateVersion++; state.value = value;
      if (value.phase === 'error' || value.phase === 'off') {
        if (value.lastError) error.value = `设备音频已停止（${value.lastError}）`;
        void stop();
      }
    }));
    subscriptions.push(client.on('intercom.audio', value => {
      if (owner !== client || !active.value || held.value || pending.value || releasing.value || disposed) return;
      try { playback?.feed(value); }
      catch (e) { error.value = describeError(e); void stop(); }
    }));
  }, { immediate: true });
  const hide = () => { if (document.hidden) void stop(); };
  const blur = () => { void release(); };
  document.addEventListener('visibilitychange', hide); window.addEventListener('blur', blur);
  onBeforeUnmount(() => {
    disposed = true; void stop(); subscriptions.forEach(off => off());
    document.removeEventListener('visibilitychange', hide); window.removeEventListener('blur', blur);
  });
  return { state, active, held, pending, releasing, busy, error, supported, canStart, microphoneSupported, label, start, stop, press, release };
}
