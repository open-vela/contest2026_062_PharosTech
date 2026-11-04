import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { createApp, h, nextTick, reactive } from 'vue';
import { createPinia } from 'pinia';
const fixture = vi.hoisted(() => ({ session: undefined as any, look: { x: 0, y: 0 } }));
vi.mock('../src/stores/session', () => ({ useSessionStore: () => fixture.session }));
vi.mock('../src/stores/eye', () => ({ useEyeStore: () => ({ ready: true, nativeCore: true, lightPreview: 55, lastState: null }) }));
vi.mock('../src/composables/usePageVisible', () => ({ useVisiblePoll: () => {} }));
vi.mock('../src/views/services/features/CameraIntercom.vue', () => ({ default: { render: () => null } }));
vi.mock('../src/lib/cameraPlayer', () => ({ CameraPlayer: class { push = vi.fn(); close = vi.fn(); } }));
vi.mock('@nyabula/ui', async () => {
  const { defineComponent, h } = await import('vue');
  return {
    UiIcon: { render: () => h('i') }, describeError: (e: any) => e.message || String(e),
    EyeCanvas: defineComponent({ props: ['eyeState', 'toyMode', 'toyScope', 'clickHold'], emits: ['look', 'ready'], setup: (p, c) => () => h('canvas', { class: 'eye-canvas', 'data-toy-mode': String(p.toyMode), 'data-toy-scope': p.toyScope, 'data-click-hold': String(p.clickHold), onPointerdown: () => c.emit('look', fixture.look) }) }),
    NkHeader: defineComponent({ props: ['title', 'subtitle'], setup: p => () => h('header', [p.title, p.subtitle]) }),
    MdButton: defineComponent({ props: { disabled: Boolean }, setup: (p, c) => () => h('button', { ...c.attrs, disabled: p.disabled }, c.slots.default?.()) }),
    NkSliderRow: defineComponent({ props: ['modelValue', 'disabled', 'title', 'min', 'max'], emits: ['update:modelValue'], setup: (p, c) => () => h('input', { type: 'range', min: p.min, max: p.max, value: p.modelValue, disabled: p.disabled, 'aria-label': p.title, onInput: (e: any) => c.emit('update:modelValue', Number(e.target.value)) }) }),
  };
});
import CameraFeature from '../src/views/services/features/CameraFeature.vue';

let app: ReturnType<typeof createApp> | undefined, host: HTMLElement, client: any;
const initial = () => ({ available: true, running: false, phase: 'idle', width: 640, height: 480, frames: 0, lastFrameMs: 0, readers: 0, bufferBytes: 0, lastError: 0 });
const png = btoa(String.fromCharCode(137, 80, 78, 71, 13, 10, 26, 10));
function makeClient() {
  const listeners = new Map<string, Function>();
  return {
    capabilities: ['core.camera-v1'], state: 'connected', listeners,
    on: (topic: string, cb: Function) => { listeners.set(topic, cb); return () => listeners.delete(topic); },
    request: vi.fn(async (topic: string, data: any = {}) => {
      if (topic === 'camera.preview.start') {
        listeners.get('camera.segment')?.({ kind:'init', sequence:0, generation:1, offset:0, total:2, data:btoa('ab') });
        listeners.get('camera.segment')?.({ kind:'fragment', sequence:1, generation:1, offset:0, total:2, data:btoa('cd'), key:true });
      }
      if (topic === 'camera.status') return initial();
      if (topic === 'camera.calibration.get') return { revision: 0, calibrated: false };
      if (topic === 'camera.calibration.fit' || topic === 'camera.calibration.set') return { ...data, calibrated: true, acceptable: true, rmsError: .01, maxError: .02, revision: topic.endsWith('set') ? 1 : 0 };
      if (topic === 'camera.aim') return { gaze_x: data.x, gaze_y: data.y };
      if (topic === 'camera.photo.capture') return { photoId: 1, bytes: 8, mime: 'image/png' };
      if (topic === 'camera.photo.read') return { offset: 0, total: 8, data: png };
      return { subscribed: topic.endsWith('start') };
    }),
  };
}
async function flush() { for (let i = 0; i < 12; i++) { await Promise.resolve(); await nextTick(); } }
async function mount() { app = createApp({ render: () => h(CameraFeature, { type: 'camera', ff: 'desktop' }) }); app.use(createPinia()); app.mount(host); await flush(); }
const button = (name: string) => [...host.querySelectorAll<HTMLButtonElement>('button')].find(el => el.textContent?.trim() === name)!;
beforeEach(() => {
  vi.stubGlobal('MediaSource', class {});
  vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:camera-test');
  vi.spyOn(URL, 'revokeObjectURL').mockImplementation(() => {});
  Object.defineProperty(document, 'hidden', { configurable: true, value: false });
  host = document.createElement('div'); document.body.append(host); client = makeClient();
  fixture.session = reactive({ connected: true, isOwner: true, role: 'owner', client });
});
afterEach(() => { app?.unmount(); app = undefined; host.remove(); vi.restoreAllMocks(); vi.unstubAllGlobals(); });

it('never starts the camera merely by opening the feature and stops a preview on exit', async () => {
  await mount(); expect(client.request.mock.calls.map((c: any) => c[0])).toEqual(['camera.status', 'camera.calibration.get']);
  expect(host.querySelector('.eye-canvas')).toBeNull();
  expect(host.querySelector('.gaze-display')).toBeNull();
  button('开启画面').click(); await flush(); expect(client.request).toHaveBeenCalledWith('camera.preview.start', {});
  app!.unmount(); app = undefined; await flush(); expect(client.request).toHaveBeenCalledWith('camera.preview.stop', {});
});
it('stops on backgrounding and does not automatically restart', async () => {
  await mount(); button('开启画面').click(); await flush();
  Object.defineProperty(document, 'hidden', { configurable: true, value: true }); document.dispatchEvent(new Event('visibilitychange')); await flush();
  expect(client.request).toHaveBeenCalledWith('camera.preview.stop', {});
  Object.defineProperty(document, 'hidden', { configurable: true, value: false }); document.dispatchEvent(new Event('visibilitychange')); await flush();
  expect(client.request.mock.calls.filter((c: any) => c[0] === 'camera.preview.start')).toHaveLength(1);
});
it('downloads only an explicitly requested photo and does not send it to an agent', async () => {
  await mount(); button('拍照').click(); await flush();
  expect(client.request).toHaveBeenCalledWith('camera.photo.capture', {}, { timeoutMs: 15000 });
  expect(host.querySelector('a[download]')?.getAttribute('href')).toBe('blob:camera-test');
  expect(client.request.mock.calls.some((c: any) => /agent|mimo/.test(c[0]))).toBe(false);
});
it('collects five manual camera/eye pairs and saves through the current revision', async () => {
  await mount(); button('开始五点校准').click(); await flush();
  expect(client.request).toHaveBeenCalledWith('camera.preview.start', {});
  expect(host.querySelector('.camera-layout.calibrating')).not.toBeNull();
  const eyes = host.querySelector('canvas.eye-canvas')!;
  expect(eyes.getAttribute('data-toy-mode')).toBe('false')
  expect(eyes.getAttribute('data-click-hold')).toBe('true');
  expect(eyes.getAttribute('data-toy-scope')).toBe('canvas');
  expect(host.querySelector('input[type=range]')).toBeNull();
  const frame = host.querySelector('.viewfinder') as HTMLElement;
  vi.spyOn(frame, 'getBoundingClientRect').mockReturnValue({ left: 0, top: 0, width: 640, height: 480 } as DOMRect);
  for (const [u, v, x, y, name] of [[.1, .1, -80, -80, '左上'], [.9, .1, 80, -80, '右上'], [.9, .9, 80, 80, '右下'], [.1, .9, -80, 80, '左下'], [.5, .5, 0, 0, '中心']] as const) {
    frame.dispatchEvent(new MouseEvent('click', { clientX: u * 640, clientY: v * 480 }));
    fixture.look = { x: x / 100, y: y / 100 };
    eyes.dispatchEvent(new Event('pointerdown')); await flush();
    button(`记录${name}点`).click(); await flush();
  }
  expect(host.textContent).toContain('五点拟合完成');
  button('保存校准').click(); await flush();
  const save = client.request.mock.calls.find((c: any) => c[0] === 'camera.calibration.set');
  expect(save[1].points).toHaveLength(5); expect(save[1].revision).toBe(0); expect(save[1].mode).toBe(6);
  expect(host.textContent).toContain('校准已保存');
  expect(host.querySelector('.eye-canvas')).toBeNull();
});
it('does not capture or fetch private status as a guest or on older firmware', async () => {
  fixture.session.isOwner = false; fixture.session.role = 'guest'; await mount();
  expect(client.request).not.toHaveBeenCalled(); expect(button('拍照').disabled).toBe(true);
});
it('keeps the clicked target while moving to the record button and releases it on cancel', async () => {
  vi.useFakeTimers();
  try {
    await mount(); button('开始五点校准').click(); await flush();
    const eyes = host.querySelector('canvas.eye-canvas')!;
    fixture.look = { x: .6, y: -.4 }; eyes.dispatchEvent(new Event('pointerdown')); await flush();
    expect(client.request).toHaveBeenCalledWith('camera.aim', { x: .6, y: -.4, hold_ms: 3000 });
    fixture.look = { x: -.9, y: .9 }; eyes.dispatchEvent(new Event('pointermove')); eyes.dispatchEvent(new Event('pointerleave')); await flush();
    await vi.advanceTimersByTimeAsync(1100); await flush();
    const aims = client.request.mock.calls.filter((c: any) => c[0] === 'camera.aim');
    expect(aims.at(-1)[1]).toEqual({ x: .6, y: -.4, hold_ms: 3000 });
    button('取消').click(); await flush();
    expect(client.request).toHaveBeenLastCalledWith('camera.aim', { x: 0, y: 0, hold_ms: 0 });
    const count = client.request.mock.calls.length;
    await vi.advanceTimersByTimeAsync(4000); await flush();
    expect(client.request.mock.calls).toHaveLength(count);
    expect(host.querySelector('.eye-canvas')).toBeNull();
  } finally { vi.useRealTimers(); }
});
it('keeps a new device preview active when an old start response arrives late', async () => {
  let resolveStart!: (v: unknown) => void;
  const original = client.request.getMockImplementation();
  client.request.mockImplementation((topic: string, data: any) => topic === 'camera.preview.start' ? new Promise(resolve => { resolveStart = resolve; }) : original(topic, data));
  await mount(); button('开启画面').click(); await flush();
  const replacement = makeClient(); fixture.session.client = replacement; await flush();
  button('开启画面').click(); await flush();
  resolveStart({ subscribed: true }); await flush();
  expect(button('停止预览')).toBeTruthy(); expect(replacement.request.mock.calls.filter((c: any) => c[0] === 'camera.preview.stop')).toHaveLength(0);
});
