import { afterEach, expect, it, vi } from 'vitest';
import { createApp, h, nextTick } from 'vue';
import { readFileSync, readdirSync } from 'node:fs';
import { resolve } from 'node:path';
vi.mock('@nyabula/ui', () => ({ UiIcon: { render: () => h('i') } }));
vi.mock('../src/stores/session', () => ({ useSessionStore: () => ({ connected: true }) }));
vi.mock('../src/lib/panelVersion', () => ({ entryOf: () => 'new', runningEntry: () => 'old', isStale: () => true }));
import Banner from '../src/components/PanelUpdateBanner.vue';
afterEach(() => { vi.unstubAllGlobals(); document.body.innerHTML = ''; });
it('shows the update action without inheriting shell banner styles', async () => {
 vi.stubGlobal('__NYA_DEVICE__', true);
 vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok: true, text: async () => 'new' }));
 const host = document.createElement('div'); document.body.append(host);
 const app = createApp(Banner); app.mount(host);
 await new Promise(r => setTimeout(r, 0)); await nextTick();
 expect(host.querySelector('[role="status"]')?.classList.contains('panel-update')).toBe(true);
 expect(host.querySelector('[role="status"]')?.classList.contains('banner')).toBe(false);
 expect(host.querySelector('button')?.textContent).toBe('刷新');
 expect(host.textContent).toContain('面板已更新');
 app.unmount();
});
it('covers every classic, custom and bare shell', () => {
 const roots = ['src/layouts', 'src/skins'];
 let count = 0;
 for (const root of roots) for (const entry of readdirSync(root, { recursive: true }).map(String)) {
  if (!entry.endsWith('Shell.vue')) continue;
  const text = readFileSync(resolve(root, entry), 'utf8');
  expect(text.match(/<PanelUpdateBanner\s*\/>/g), entry).toHaveLength(1);
  count++;
 }
 expect(count).toBe(8);
});
