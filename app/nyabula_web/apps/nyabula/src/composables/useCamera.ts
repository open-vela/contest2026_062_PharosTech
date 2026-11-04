import { computed, onBeforeUnmount, ref, shallowRef, watch } from 'vue';
import { describeError } from '@nyabula/ui';
import { useSessionStore } from '../stores/session';
import { CameraAssembler, CAMERA_MAX_PHOTO, decodeCameraChunk, isCameraStatus, isCameraCalibration,
  type CameraStatus, type CameraCalibration, type CalibrationPoint } from '../lib/cameraMedia';
import { CameraPlayer } from '../lib/cameraPlayer';
import { useVisiblePoll } from './usePageVisible';

export function useCamera() {
  const session = useSessionStore();
  const video = shallowRef<HTMLVideoElement | null>(null);
  const status = shallowRef<CameraStatus>();
  const calibration = shallowRef<CameraCalibration>();
  const fit = shallowRef<CameraCalibration>();
  const busy = ref(false), streaming = ref(false), received = ref(0);
  const error = ref(''), readError = ref(''), photoUrl = ref('');
  const actualGaze = ref<{ x: number; y: number }>();
  const supported = computed(() => session.client?.capabilities.includes('core.camera-v1') === true);
  const canUse = computed(() => session.connected && session.isOwner && supported.value && !busy.value && status.value?.phase !== 'stopping');
  const canPreview = typeof MediaSource !== 'undefined';
  const label = computed(() => !session.connected ? '设备离线' : !session.isOwner ? '请以主人身份连接' : !supported.value ? '需要相机版固件' :
    status.value?.phase === 'error' ? `采集异常 · ${status.value.lastError}` : status.value?.phase === 'stopping' ? '正在释放摄像头' : streaming.value ? received.value ? '画面已连接' : '正在等待第一帧' :
    status.value?.running ? '摄像头正在使用' :
    status.value?.available === false ? '摄像头未就绪' : '摄像头待命');
  let epoch = 0, refreshing = false, closed = false;
  let offSegment: (() => void) | undefined, offError: (() => void) | undefined;
  let player: CameraPlayer | null = null;
  let aimClient: typeof session.client = null;
  const connection = computed(() => session.client);
  function releaseHeldAim() {
    const client = aimClient; aimClient = null;
    if (client?.state === 'connected') void client.request('camera.aim', { x: 0, y: 0, hold_ms: 0 }).catch(() => {});
  }
  const assembler = new CameraAssembler();
  const clearPhoto = () => { if (photoUrl.value) URL.revokeObjectURL(photoUrl.value); photoUrl.value = ''; };

  async function stop() {
    const client = session.client;
    const active = streaming.value;
    streaming.value = false; received.value = 0; assembler.reset(); player?.close();
    if (active && client?.state === 'connected') {
      try { await client.request('camera.preview.stop', {}); }
      catch (e) { if (!closed && client === session.client) error.value = describeError(e); }
    }
  }
  function playbackError(message: string) { error.value = message; void stop(); }
  watch(video, element => {
    player?.close(); player = element ? new CameraPlayer(element, playbackError) : null;
  });

  async function refresh() {
    if (!session.connected || !session.isOwner || !supported.value || refreshing || busy.value) return;
    const client = session.client!, current = epoch; refreshing = true;
    try {
      const value = await client.request('camera.status', {});
      if (!isCameraStatus(value)) throw Error('设备返回了无效的相机状态');
      if (current === epoch && !closed) { status.value = value; readError.value = ''; }
      if (!calibration.value) {
        const next = await client.request('camera.calibration.get', {});
        if (!isCameraCalibration(next)) throw Error('校准记录无效');
        if (current === epoch && !closed) calibration.value = next;
      }
    } catch (e) { if (current === epoch && !closed) readError.value = describeError(e); }
    finally { refreshing = false; }
  }
  async function start() {
    if (!canUse.value || !canPreview || streaming.value) return;
    const client = session.client!, current = epoch;
    busy.value = true; error.value = ''; received.value = 0; assembler.reset();
    // Listen before the response: the first event can follow it in the same tick.
    streaming.value = true;
    try {
      await client.request('camera.preview.start', {});
      if (current !== epoch || closed || document.hidden) {
        if (current === epoch) streaming.value = false;
        await client.request('camera.preview.stop', {});
      }
    } catch (e) { if (current === epoch && !closed) { streaming.value = false; error.value = describeError(e); } }
    finally { if (current === epoch && !closed) { busy.value = false; void refresh(); } }
  }
  async function photograph() {
    if (!canUse.value) return;
    const client = session.client!, current = epoch;
    busy.value = true; error.value = '';
    try {
      const meta = await client.request('camera.photo.capture', {}, { timeoutMs: 15000 });
      const size = meta.bytes, id = meta.photoId;
      if (typeof size !== 'number' || !Number.isInteger(size) || size <= 0 || size > CAMERA_MAX_PHOTO ||
          typeof id !== 'number' || !Number.isSafeInteger(id) || id <= 0 || meta.mime !== 'image/png') throw Error('照片信息无效');
      const bytes = new Uint8Array(size);
      let offset = 0;
      while (offset < size) {
        if (current !== epoch || closed) return;
        const value = await client.request('camera.photo.read', { photoId: id, offset });
        const part = decodeCameraChunk(value, CAMERA_MAX_PHOTO);
        if (part.offset !== offset || part.total !== size) throw Error('照片在下载期间已更新，请重新拍照');
        bytes.set(part.bytes, offset); offset += part.bytes.length;
        if (offset < size) await new Promise(resolve => setTimeout(resolve, 60));
      }
      if (current !== epoch || closed) return;
      if ([137, 80, 78, 71, 13, 10, 26, 10].some((b, i) => bytes[i] !== b)) throw Error('照片格式无效');
      clearPhoto(); photoUrl.value = URL.createObjectURL(new Blob([bytes], { type: 'image/png' }));
    } catch (e) { if (current === epoch && !closed) error.value = describeError(e); }
    finally { if (current === epoch && !closed) { busy.value = false; void refresh(); } }
  }
  async function calibrate(points: CalibrationPoint[], save: boolean) {
    if (!canUse.value || points.length !== 5) return false;
    const client = session.client!, current = epoch;
    busy.value = true; error.value = '';
    try {
      const value = await client.request(save ? 'camera.calibration.set' : 'camera.calibration.fit', {
        width: 640, height: 480, mode: 6, points, revision: calibration.value?.revision ?? 0,
      });
      if (!isCameraCalibration(value)) throw Error('设备返回了无效的校准结果');
      if (current !== epoch || closed) return false;
      fit.value = value;
      if (save) calibration.value = value;
      return true;
    } catch (e) { if (current === epoch && !closed) error.value = describeError(e); return false; }
    finally { if (current === epoch && !closed) busy.value = false; }
  }
  async function aim(x: number, y: number, holdMs = 3000) {
    if (!canUse.value && !(holdMs === 0 && session.connected && session.isOwner && supported.value)) return false;
    const client = session.client!, current = epoch;
    try {
      aimClient = holdMs > 0 ? client : null;
      const state = await client.request('camera.aim', { x, y, hold_ms: holdMs });
      if (current !== epoch || closed) return false;
      if (typeof state.gaze_x === 'number' && typeof state.gaze_y === 'number') actualGaze.value = { x: state.gaze_x, y: state.gaze_y };
      return true;
    } catch (e) { if (current === epoch && !closed) error.value = describeError(e); return false; }
  }
  async function point(u: number, v: number) {
    if (!canUse.value || !calibration.value?.calibrated) return;
    const client = session.client!, current = epoch;
    try {
      const state = await client.request('camera.point', { u, v, width: 640, height: 480, mode: 6 });
      if (current === epoch && !closed && typeof state.gaze_x === 'number' && typeof state.gaze_y === 'number') actualGaze.value = { x: state.gaze_x, y: state.gaze_y };
    } catch (e) { if (current === epoch && !closed) error.value = describeError(e); }
  }

  watch(() => [session.client, session.connected, session.role], (_, previous) => {
    const previousClient = previous?.[0] as typeof session.client;
    if (streaming.value && previousClient?.state === 'connected') void previousClient.request('camera.preview.stop', {}).catch(() => {});
    releaseHeldAim();
    epoch++; streaming.value = false; received.value = 0; busy.value = false;
    status.value = undefined; calibration.value = undefined; fit.value = undefined; actualGaze.value = undefined;
    clearPhoto(); assembler.reset(); player?.close(); offSegment?.(); offError?.(); error.value = readError.value = '';
    const client = session.client;
    if (client) {
      offSegment = client.on('camera.segment', data => {
        if (!streaming.value || closed || client !== session.client) return;
        try { const segment = assembler.accept(data); if (segment) { player?.push(segment); if (segment.kind === 'fragment') received.value++; } }
        catch (e) { playbackError(e instanceof Error ? e.message : '画面数据异常'); }
      });
      offError = client.on('camera.error', data => {
        if (!streaming.value || closed || client !== session.client) return;
        error.value = `设备采集已停止（${data.error ?? '未知错误'}）`;
        streaming.value = false; received.value = 0; assembler.reset(); player?.close(); void refresh();
      });
    }
    void refresh();
  }, { immediate: true });
  const visibility = () => { if (document.hidden) void stop(); };
  document.addEventListener('visibilitychange', visibility);
  useVisiblePoll(() => void refresh(), 1000);
  onBeforeUnmount(() => {
    releaseHeldAim(); void stop(); closed = true; epoch++; clearPhoto(); offSegment?.(); offError?.(); player?.close();
    document.removeEventListener('visibilitychange', visibility);
  });
  return { video, status, calibration, fit, busy, streaming, received, error, readError, photoUrl, actualGaze,
    supported, canUse, canPreview, connection, label, refresh, start, stop, photograph, calibrate, aim, point };
}
