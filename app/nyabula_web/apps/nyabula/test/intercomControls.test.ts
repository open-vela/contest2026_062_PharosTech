import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { createApp, h, nextTick, reactive } from 'vue';
const fixture = vi.hoisted(() => ({ session: undefined as any, mic: undefined as any, speaker: undefined as any, receive: undefined as any, open: undefined as any }));
vi.mock('../src/stores/session', () => ({ useSessionStore: () => fixture.session }));
vi.mock('../src/lib/intercomAudio', async () => {
  const actual = await vi.importActual<any>('../src/lib/intercomAudio');
  return { ...actual, IntercomPlayback: { open: () => Promise.resolve(fixture.speaker) }, IntercomMicrophone: { open: (receive: any) => { fixture.receive = receive; return fixture.open(); } } };
});
vi.mock('@nyabula/ui', async () => {
  const { defineComponent, h } = await import('vue');
  return { UiIcon: { render: () => h('i') }, describeError: (e: any) => e.message || String(e),
    MdButton: defineComponent({ props: { disabled: Boolean }, setup: (p, c) => () => h('button', { ...c.attrs, disabled: p.disabled }, c.slots.default?.()) }) };
});
import CameraIntercom from '../src/views/services/features/CameraIntercom.vue';
let app: ReturnType<typeof createApp> | undefined, host: HTMLElement, client: any;
const state = (phase = 'listening') => ({ phase, duplex: 'push-to-talk', sampleRate: 16000, channels: 1, format: 's16le', capturedSamples: 1600, playedSamples: 0, queuedPackets: 0, level: 1000, lastError: 0 });
async function flush() { for (let i = 0; i < 15; i++) { await Promise.resolve(); await nextTick(); } }
const button = (label: string) => [...host.querySelectorAll<HTMLButtonElement>('button')].find(el => el.textContent?.trim() === label)!;
async function mount() { app = createApp({ render: () => h(CameraIntercom) }); app.mount(host); await flush(); }
beforeEach(() => {
  Object.defineProperty(window, 'isSecureContext', { configurable: true, value: true });
  Object.defineProperty(navigator, 'mediaDevices', { configurable: true, value: { getUserMedia: vi.fn() } });
  Object.defineProperty(document, 'hidden', { configurable: true, value: false });
  const listeners = new Map<string, Function>();
  client = { state: 'connected', capabilities: ['core.intercom-v1'], listeners, on: (topic: string, cb: Function) => { listeners.set(topic, cb); return () => listeners.delete(topic); },
    request: vi.fn(async (topic: string) => state(topic === 'intercom.talk.start' ? 'talking' : 'listening')) };
  fixture.session = reactive({ client, connected: true, isOwner: true, role: 'owner' });
  fixture.mic = { close: vi.fn(async () => {}) }; fixture.open = vi.fn(async () => fixture.mic);
  fixture.speaker = { feed: vi.fn(), clear: vi.fn(), close: vi.fn(async () => {}) };
  host = document.createElement('div'); document.body.append(host);
});
afterEach(() => { app?.unmount(); app = undefined; host.remove(); vi.restoreAllMocks(); });

it('opens no microphone on entry or listen, and binds capture to an explicit held button', async () => {
  await mount(); expect(client.request).not.toHaveBeenCalled();
  button('开启对讲').click(); await flush(); expect(fixture.open).not.toHaveBeenCalled();
  const hold = host.querySelector('.push-to-talk')!;
  hold.dispatchEvent(new KeyboardEvent('keydown', { key: ' ', code: 'Space' })); await flush();
  expect(client.request).toHaveBeenCalledWith('intercom.talk.start', {});
  fixture.receive(Uint8Array.from([232, 3])); await flush();
  expect(client.request).toHaveBeenCalledWith('intercom.audio', { sequence: 0, data: '6AM=' }, { timeoutMs: 3000 });
  hold.dispatchEvent(new KeyboardEvent('keyup', { key: ' ', code: 'Space' })); await flush();
  expect(fixture.mic.close).toHaveBeenCalledWith(true);
  expect(client.request).toHaveBeenCalledWith('intercom.talk.stop', {});
});
it('closes a late microphone permission result without starting device playback', async () => {
  let resolve!: (value: unknown) => void; fixture.open = vi.fn(() => new Promise(r => { resolve = r; }));
  await mount(); button('开启对讲').click(); await flush(); const hold = host.querySelector('.push-to-talk')!;
  hold.dispatchEvent(new KeyboardEvent('keydown', { key: ' ', code: 'Space' })); await flush();
  hold.dispatchEvent(new KeyboardEvent('keyup', { key: ' ', code: 'Space' })); await flush();
  resolve(fixture.mic); await flush(); expect(fixture.mic.close).toHaveBeenCalledWith(false);
  expect(client.request.mock.calls.some((c: any) => c[0] === 'intercom.talk.start')).toBe(false);
});
it('cleans up both audio directions on backgrounding and on disconnect', async () => {
  await mount(); button('开启对讲').click(); await flush();
  Object.defineProperty(document, 'hidden', { configurable: true, value: true }); document.dispatchEvent(new Event('visibilitychange')); await flush();
  expect(client.request).toHaveBeenCalledWith('intercom.stop', {}); expect(fixture.speaker.close).toHaveBeenCalled();
  Object.defineProperty(document, 'hidden', { configurable: true, value: false });
  button('开启对讲').click(); await flush(); fixture.session.connected = false; await flush();
  expect(button('开启对讲').disabled).toBe(true);
});
it('stops instead of accumulating unbounded microphone packets', async () => {
  await mount(); button('开启对讲').click(); await flush();
  host.querySelector('.push-to-talk')!.dispatchEvent(new KeyboardEvent('keydown', { key: ' ', code: 'Space' })); await flush();
  for (let i = 0; i < 5; i++) fixture.receive(new Uint8Array(3200));
  await flush(); expect(host.textContent).toContain('网络发送过慢'); expect(client.request).toHaveBeenCalledWith('intercom.stop', {});
});
it('keeps unsupported and guest sessions read-only', async () => {
  fixture.session.isOwner = false; fixture.session.role = 'guest'; await mount();
  expect(button('开启对讲').disabled).toBe(true); expect(client.request).not.toHaveBeenCalled();
});
