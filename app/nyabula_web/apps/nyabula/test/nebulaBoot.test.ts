import { afterEach, expect, it, vi } from 'vitest';
import { createApp, h } from 'vue';
import { createPinia } from 'pinia';
import NebulaBoot from '../../../packages/ui/src/skins/nebula/NebulaBoot.vue';
import { useLoadingStore } from '../../../packages/ui/src/stores/loading';
const calls = vi.hoisted(() => ({ mode: vi.fn(), bare: vi.fn(), asleep: vi.fn(), copy: vi.fn() }));
vi.mock('@nyabula/eye-engine', () => ({ EyeEngine: class {
 static forCanvas() { return { irisHexL: '#30ffa6', irisHexR: '#30ffa6', lightCur: .8, copyPresentationFrame: calls.copy }; }
 start() {} stop() {} destroy() {} setZzzEnabled() {} setIris() {} setLight() {}
 capturePresentation() { return { params: {}, irisLeft: '#30ffa6', irisRight: '#30ffa6' }; }
 setMode = calls.mode; setBare = calls.bare; startAsleep = calls.asleep;
} }));
let cleanup = () => {};
afterEach(() => { cleanup(); vi.useRealTimers(); vi.unstubAllGlobals(); vi.clearAllMocks(); });
function mount(pending = false) {
 vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'Date', 'performance'] });
 vi.stubGlobal('requestAnimationFrame', (fn: FrameRequestCallback) => setTimeout(() => fn(performance.now()), 16));
 vi.stubGlobal('cancelAnimationFrame', clearTimeout);
 vi.stubGlobal('matchMedia', () => ({ matches: false }));
 const shell = document.createElement('div'); shell.dataset.bootPending = String(pending); document.body.append(shell);
 const host = document.createElement('div'); document.body.append(host);
 const app = createApp({ render: () => h(NebulaBoot) }); const pinia = createPinia(); app.use(pinia); app.mount(host);
 useLoadingStore(pinia).finishBoot();
 cleanup = () => { app.unmount(); host.remove(); shell.remove(); };
 function destination() {
  shell.dataset.bootPending = 'false'; const a = document.createElement('div');a.setAttribute('data-eyes-anchor','');
  const source = document.createElement('canvas');source.width=1520;source.height=840;
  source.getBoundingClientRect = () => ({left:120,top:240,width:760,height:420}) as DOMRect;
  a.append(source);shell.append(a);
 }
 return { host, destination };
}
it('does not fade out while the initial device route is still pending', async () => {
 const t = mount(true); await vi.advanceTimersByTimeAsync(5000);
 expect(t.host.querySelector('.nb-boot.dusk')).not.toBeNull(); expect(calls.asleep).not.toHaveBeenCalled();
 t.destination(); await vi.advanceTimersByTimeAsync(100);
 expect(calls.asleep).toHaveBeenCalledOnce();
 expect(calls.copy).not.toHaveBeenCalled();
 await vi.advanceTimersByTimeAsync(1650);
 expect(t.host.textContent).toContain('你好呀');expect(calls.mode.mock.calls.flat()).toEqual(['sleepy', 'idle']);
 expect(calls.copy).not.toHaveBeenCalled();
 await vi.advanceTimersByTimeAsync(900);
 expect(t.host.querySelector('.nb-boot.fly')).not.toBeNull();
 expect(t.host.querySelector('.no-anchor')).toBeNull();
 expect(calls.copy.mock.calls.at(-1)?.[4]).toBeGreaterThan(0);
 expect(calls.copy.mock.calls.at(-1)?.[4]).toBeLessThan(1);
 expect(document.documentElement.classList.contains('nb-eyes-handoff')).toBe(true);
 await vi.advanceTimersByTimeAsync(1100);
 expect(t.host.querySelector('.nb-boot')).toBeNull();
 expect(document.documentElement.classList.contains('nb-eyes-handoff')).toBe(false);
});
it('keeps the original intro on real non-home pages and exits without a destination', async () => {
 const t = mount(); await vi.advanceTimersByTimeAsync(2700);
 expect(t.host.querySelector('.no-anchor.fly')).not.toBeNull();
 await vi.advanceTimersByTimeAsync(1100); expect(t.host.querySelector('.nb-boot')).toBeNull();
});

it('uses exactly the same opening state transitions and timing on home and other pages', async () => {
 async function sequence(home: boolean) {
  const t = mount(); if (home) t.destination();
  const events: { mode: string; time: number }[] = [];
  calls.mode.mockImplementation(mode => { events.push({ mode, time: performance.now() }); });
  await vi.advanceTimersByTimeAsync(1550);
  expect(t.host.querySelector('.awake')).toBeNull();
  expect(calls.copy).not.toHaveBeenCalled();
  await vi.advanceTimersByTimeAsync(150);
  expect(t.host.querySelector('.awake')).not.toBeNull();
  cleanup(); vi.useRealTimers(); vi.unstubAllGlobals(); vi.clearAllMocks();
  return events;
 }
 const home = await sequence(true), other = await sequence(false);
 expect(home).toEqual(other);
 expect(home.map(e => e.mode)).toEqual(['sleepy', 'idle']);
 expect(home[1].time - home[0].time).toBe(900);
});
