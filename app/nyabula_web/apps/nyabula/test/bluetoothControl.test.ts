import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createApp, h, nextTick, reactive } from 'vue';
const state = vi.hoisted(() => ({ session: undefined as any, poll: undefined as any }));
vi.mock('../src/stores/session', () => ({ useSessionStore: () => state.session }));
vi.mock('../src/composables/usePageVisible', () => ({ useVisiblePoll: (poll: () => void) => { state.poll = poll; } }));
vi.mock('@nyabula/ui', async () => {
  const { defineComponent, h } = await import('vue');
  return {
    MdButton: defineComponent({ props: ['disabled'], setup: (props, ctx) => () => h('button', { ...ctx.attrs, disabled: props.disabled }, ctx.slots.default?.()) }),
    UiIcon: defineComponent({ setup: () => () => h('span') }),
    describeError: (error: Error) => error.message,
  };
});
import BluetoothControl from '../src/views/services/features/BluetoothControl.vue';
const A = 'AA:00:00:00:00:01', B = 'BB:00:00:00:00:02';
function snapshot(selected = A) {
  return {
    state: 'ready', name: 'Nyabula', address: '00', error: 0, discoverable: false, bondsPersistent: true, maxConnections: 2,
    connected: { name: selected === A ? 'Laptop' : 'Phone', address: selected },
    connections: [A, B].map(address => ({ name: address === A ? 'Laptop' : 'Phone', address, connected: true, selected: address === selected, a2dpState: 'open', avrcpConnected: true, call: 'idle' })),
    a2dp: { state: 'open', codec: 'SBC', sampleRate: 44100, channels: 2 },
    avrcp: { connected: true, playing: true, title: selected === A ? 'Laptop song' : 'Phone song', pending: false, controlError: 0 },
    hfp: { state: 'connected', call: 'idle', codec: 'none', audio: false },
    audio: { owner: 'none', error: 0, mediaPackets: 0, scoRxPackets: 0, scoTxPackets: 0, bufferMs: 0 },
  };
}
function deferred() { let resolve!: (v: any) => void; const promise = new Promise<any>(r => { resolve = r; }); return { promise, resolve }; }
const flush = async () => { for (let i = 0; i < 8; i++) await Promise.resolve(); await nextTick(); };
let host: HTMLElement, app: ReturnType<typeof createApp>;
const button = (text: string) => [...host.querySelectorAll('button')].find(b => b.textContent === text)!;
beforeEach(async () => {
  state.session = reactive({ connected: true, isOwner: true, client: {}, request: vi.fn().mockImplementation((topic: string) => Promise.resolve(topic === 'bt.devices' ? { devices: [] } : snapshot())) });
  host = document.createElement('div'); document.body.append(host);
  app = createApp({ render: () => h(BluetoothControl, { mode: 'media' }) }); app.mount(host); await flush();
});
afterEach(() => { app.unmount(); host.remove(); });
describe('Bluetooth source ownership', () => {
  it('shows both connections and addresses media controls to the selected source', async () => {
    expect(host.querySelectorAll('.connection-list .device-row')).toHaveLength(2);
    expect(host.textContent).toContain('当前音源'); expect(host.textContent).toContain('待命');
    button('暂停').click(); await flush();
    expect(state.session.request).toHaveBeenCalledWith('bt.media.pause', { address: A });
  });
  it('selects the named source and ignores an older poll response', async () => {
    const stale = deferred();
    state.session.request.mockImplementation((topic: string) => topic === 'bt.status' ? stale.promise : Promise.resolve(topic === 'bt.select' ? snapshot(B) : { devices: [] }));
    state.poll(); await Promise.resolve(); button('切换音源').click(); await flush();
    expect(state.session.request).toHaveBeenCalledWith('bt.select', { address: B });
    expect(host.textContent).toContain('Phone song');
    stale.resolve(snapshot(A)); await flush(); expect(host.textContent).toContain('Phone song');
    button('暂停').click(); await flush(); expect(state.session.request).toHaveBeenCalledWith('bt.media.pause', { address: B });
  });
  it('keeps a command error visible after successful status refreshes', async () => {
    state.session.request.mockImplementation((topic: string) => topic === 'bt.media.pause' ? Promise.reject(Error('remote rejected control')) : Promise.resolve(topic === 'bt.devices' ? { devices: [] } : snapshot()));
    button('暂停').click(); await flush(); state.poll(); await flush();
    expect(host.textContent).toContain('remote rejected control');
    expect(host.textContent).toContain('操作未完成');
  });
  it('waits for the current outgoing connection before starting another', async () => {
    const next = snapshot(); next.connections = [next.connections[0]]; next.connections[0].connected = false;
    state.session.request.mockImplementation((topic: string) => Promise.resolve(topic === 'bt.devices' ? { devices: [{ address: B, name: 'Phone', bonded: true, connected: false }] } : next));
    state.poll(); await flush(); expect(button('连接').disabled).toBe(true);
  });
  it('blocks switching during a call and repeated keys while awaiting completion', async () => {
    const next = snapshot(); next.hfp.call = 'active'; next.avrcp.pending = true;
    state.session.request.mockImplementation((topic: string) => Promise.resolve(topic === 'bt.devices' ? { devices: [] } : next));
    state.poll(); await flush(); expect(button('切换音源').disabled).toBe(true); expect(button('正在发送…').disabled).toBe(true);
  });
});
