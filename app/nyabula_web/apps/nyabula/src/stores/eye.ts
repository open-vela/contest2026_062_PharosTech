/* Eye domain: latest authoritative eye.state plus optimistic mode/scene
 * intent. Subscribes when the session client changes. */
import { defineStore } from 'pinia';
import { computed, ref, shallowRef, watch } from 'vue';
import { coreEyeState, isCoreEyeSnapshot, type EyeState } from '@nyabula/eye-engine';
import { useSessionStore } from './session';
import { useToastStore } from '@nyabula/ui';

export type SceneStyle = 'full' | 'minimal';

export const MODE_LABELS: Record<string, string> = {
  idle: '待机', curious: '好奇', happy: '开心', processing: '思考', star: '星星眼', heart: '爱心眼',
  sleepy: '困倦', sleep: '睡眠', angry: '生气', sad: '委屈', surprise: '惊讶', dizzy: '眩晕', derp: '呆萌',
};

export const SCENE_META: Record<string, { label: string; icon: string; group: string }> = {
  music: { label: '音乐', icon: 'music_note', group: '媒体' },
  audio: { label: '音频路由', icon: 'volume_up', group: '媒体' },
  eq: { label: '均衡器', icon: 'tune', group: '媒体' },
  subwoofer: { label: '低音炮', icon: 'subwoofer', group: '媒体' },
  caption: { label: '字幕', icon: 'article', group: '媒体' },
  call: { label: '通话', icon: 'call', group: '通讯' },
  presence: { label: '在场', icon: 'presence', group: '通讯' },
  companion: { label: '陪伴', icon: 'companion', group: '通讯' },
  timer: { label: '倒计时', icon: 'timer', group: '时间' },
  stopwatch: { label: '秒表', icon: 'schedule', group: '时间' },
  alarm: { label: '闹钟', icon: 'alarm', group: '时间' },
  calendar: { label: '日历', icon: 'calendar', group: '时间' },
  'sleep-timer': { label: '睡眠定时', icon: 'moon', group: '时间' },
  weather: { label: '天气', icon: 'cloud', group: '信息' },
  briefing: { label: '简报', icon: 'article', group: '信息' },
  task: { label: '任务', icon: 'task_alt', group: '信息' },
  memory: { label: '记忆', icon: 'memory', group: '信息' },
  health: { label: '健康', icon: 'heart_rate', group: '信息' },
  battery: { label: '电量', icon: 'battery', group: '系统' },
  network: { label: '网络', icon: 'wifi', group: '系统' },
  devices: { label: '设备', icon: 'devices', group: '系统' },
  system: { label: '系统', icon: 'system', group: '系统' },
  privacy: { label: '隐私', icon: 'privacy', group: '系统' },
  identity: { label: '身份', icon: 'identity', group: '系统' },
  home: { label: '家居', icon: 'home', group: '系统' },
  sleep: { label: '休眠', icon: 'moon', group: '系统' },
  pairing: { label: '配对', icon: 'qr_code', group: '系统' },
};

export const SCENE_GROUPS = ['媒体', '通讯', '时间', '信息', '系统'];

export const useEyeStore = defineStore('eye', () => {
  const session = useSessionStore();
  const toast = useToastStore();
  const lastState = shallowRef<EyeState | null>(null);
  const nativeCore = ref(false);
  const ready = computed(() => session.connected && (!nativeCore.value || lastState.value !== null));
  const pendingMode = ref<string | null>(null);
  const pendingScene = ref<string | null | undefined>(undefined);
  const sceneStyle = ref<SceneStyle>('full');
  /** Ambient light 0..100. On a native Core it mirrors the device value and
   *  is changed through `eyes.ambient`, so the preview never shows a pupil the
   *  hardware does not; legacy links have no topic and keep it preview-only. */
  const lightPreview = ref(55);
  /** Commands sent but not answered yet. A broadcast that arrives meanwhile was
   *  produced before the device applied them and must not undo the choice. */
  let inflight = 0;
  /** Ambient level waiting to be sent / being sent (see setAmbient). */
  let nextAmbient: number | null = null;
  let sendingAmbient = false;

  let unsub: (() => void) | null = null;
  function receiveState(data: Record<string, unknown>) {
    if (data.schema === 'nyabula.eye.v1') {
      if (!isCoreEyeSnapshot(data)) return;
      if (nativeCore.value && lastState.value?.seq !== undefined && data.seq < lastState.value.seq) return;
      nativeCore.value = true;
      lastState.value = coreEyeState(data);
    } else {
      lastState.value = data as unknown as EyeState;
    }
    const ex = lastState.value?.expression;
    const sc = lastState.value?.scene;
    // The device is authoritative, but only once it has seen the command: keep
    // the optimistic choice while its request is in flight unless this state
    // already confirms it.
    if (inflight === 0 || ex?.mode === pendingMode.value) pendingMode.value = null;
    if (inflight === 0 || (sc?.type ?? null) === pendingScene.value) pendingScene.value = undefined;
    if (nativeCore.value && typeof ex?.lightLevel === 'number' && nextAmbient === null && !sendingAmbient) {
      lightPreview.value = Math.round(ex.lightLevel * 100);
    }
    if (sc?.style === 'full' || sc?.style === 'minimal') sceneStyle.value = sc.style;
  }
  watch(
    () => session.client,
    (c) => {
      unsub?.();
      unsub = null;
      lastState.value = null;
      nativeCore.value = false;
      if (!c) return;
      unsub = c.on('eye.state', receiveState);
    },
    { immediate: true },
  );
  watch(() => session.state, state => {
    const client = session.client;
    if (state !== 'connected') {
      lastState.value = null;
      pendingMode.value = null;
      pendingScene.value = undefined;
      return;
    }
    const hasEye = client?.capabilities.includes('eyes.native-v1') === true;
    nativeCore.value = hasEye || client?.capabilities.includes('core.product-v1') === true;
    if (!hasEye) return;
    void session.request('eyes.state.get').then(data => {
      if (client === session.client) receiveState(data);
    }).catch(e => {
      if (client !== session.client) return;
      // A compiled capability can be present while its display service is stopped.
      // The preview already presents that state; it is not a transport failure.
      if (e?.code === 'ENODEV' || e?.code === 'EUNAVAILABLE') return;
      toast.error(e, '读取眼睛状态失败');
    });
  }, { immediate:true });

  const activeMode = computed(() => pendingMode.value ?? lastState.value?.expression?.mode ?? 'idle');
  const activeScene = computed<string | null>(() =>
    pendingScene.value !== undefined ? pendingScene.value : lastState.value?.scene?.type ?? null,
  );

  async function setMode(mode: string): Promise<void> {
    const prev = pendingMode.value;
    pendingMode.value = mode;
    if (!nativeCore.value) pendingScene.value = null;
    inflight++;
    try {
      await session.request(nativeCore.value ? 'eyes.expression' : 'eye.mode', nativeCore.value ? { expression: mode } : { mode });
    } catch (e) {
      pendingMode.value = prev;
      pendingScene.value = undefined;
      toast.error(e, '切换表情失败');
    } finally {
      inflight--;
    }
  }

  async function setScene(type: string | null, style: SceneStyle = sceneStyle.value, payload?: Record<string, unknown>, options?: Record<string, unknown>): Promise<void> {
    if (nativeCore.value && type === 'sleep') return setMode('sleep');
    pendingScene.value = type;
    if (type) sceneStyle.value = style;
    inflight++;
    try {
      const data: Record<string, unknown> = { type };
      if (type) {
        data.style = style;
        if (payload) data.payload = payload;
        if (options) data.options = options;
      }
      if (nativeCore.value) {
        const p = { ...payload };
        const aliases: Record<string, string> = { total_ms:'duration_ms', text:'current_line', temp:'temperature_c', condition:'weather', running:'active' };
        for (const [from,to] of Object.entries(aliases)) if (p[from] !== undefined) { p[to]=p[from]; delete p[from]; }
        const optionNames: Record<string,string> = { musicView:'music_view', battery:'battery_state', alarmCopy:'alarm_copy', call:'call_state', task:'task_state', network:'network_state', audio:'audio_route', eq:'eq_view', weather:'weather' };
        for (const [key,value] of Object.entries(options ?? {})) if (optionNames[key]) p[optionNames[key]]=value;
        if (p.audio_route === 'headphone') p.audio_route = 'headphones';
        if (p.eq_view === 'calibrate') p.eq_view = 'calibrating';
        await session.request(type ? 'eyes.scene.show' : 'eyes.scene.hide', type ? { scene:type.replaceAll('-','_'), style, payload:p } : {});
      } else await session.request('eye.scene', data);
    } catch (e) {
      pendingScene.value = undefined;
      toast.error(e, '切换场景失败');
    } finally {
      inflight--;
    }
  }

  function toggleScene(type: string): Promise<void> {
    return activeScene.value === type ? setScene(null) : setScene(type);
  }

  type Look = { x?: number; y?: number; release?: boolean; hold?: number };
  let nextLook: Look | null = null;
  let sendingLook = false;
  async function flushLook(): Promise<void> {
    if (sendingLook || !nextLook || !session.canControl) return;
    sendingLook = true;
    const client = session.client;
    const data = nextLook;
    nextLook = null;
    try {
      await session.request(nativeCore.value ? 'eyes.gaze' : 'eye.look', nativeCore.value
        ? { x:data.x ?? 0, y:data.y ?? 0, hold_ms:data.release ? 0 : Math.round((data.hold ?? 2.2)*1000) }
        : data);
    } catch { /* The connection indicator owns transport errors. */ }
    finally {
      sendingLook = false;
      if (client !== session.client || !session.canControl) nextLook = null;
      if (nextLook) void flushLook();
    }
  }
  function look(data: Look): void {
    if (!session.canControl) return;
    nextLook = data;
    void flushLook();
  }

  /* Ambient light, latest-wins like look(): a slider drag produces many values
   * and only the newest one matters. */
  async function flushAmbient(): Promise<void> {
    if (sendingAmbient || nextAmbient === null) return;
    sendingAmbient = true;
    const client = session.client;
    const level = nextAmbient;
    nextAmbient = null;
    try {
      await session.request('eyes.ambient', { level });
    } catch (e) {
      nextAmbient = null;
      toast.error(e, '设置环境光失败');
    } finally {
      sendingAmbient = false;
      if (client !== session.client) nextAmbient = null;
      if (nextAmbient !== null) void flushAmbient();
    }
  }
  /** True when the slider drives the device, false when it is preview-only. */
  const ambientOnDevice = computed(() => nativeCore.value && session.canControl);
  function setAmbient(percent: number): void {
    lightPreview.value = percent;
    if (!ambientOnDevice.value) return;
    nextAmbient = Math.min(1, Math.max(0, percent / 100));
    void flushAmbient();
  }

  return { lastState, nativeCore, ready, activeMode, activeScene, sceneStyle, lightPreview, ambientOnDevice, setMode, setScene, toggleScene, look, setAmbient };
});
