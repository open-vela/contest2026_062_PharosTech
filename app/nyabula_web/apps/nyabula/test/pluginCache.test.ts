import { beforeEach, describe, expect, it, vi } from 'vitest';
import { createPinia, setActivePinia } from 'pinia';
import { reactive } from 'vue';
const state = vi.hoisted(() => ({ session: null as any }));
vi.mock('../src/stores/session', () => ({ useSessionStore: () => state.session }));
import { usePluginsStore } from '../src/stores/plugins';

describe('plugin UI cache refresh', () => {
  let handlers: Map<string, () => void>;
  beforeEach(() => {
    setActivePinia(createPinia());
    handlers = new Map();
    state.session = reactive({ client: { on: (topic: string, handler: () => void) => { handlers.set(topic, handler); return () => {}; } }, request: vi.fn() });
  });

  it('does not unmount an existing page while ui.changed reloads its tree', async () => {
    const tree = { nyaui: 1, html: '<main>old</main>' };
    state.session.request.mockResolvedValueOnce(tree);
    const store = usePluginsStore();
    await store.loadTree('counter');
    let finish!: (value: unknown) => void;
    state.session.request.mockImplementation((topic: string) => topic === 'plugin.list'
      ? Promise.resolve({ plugins: [] }) : new Promise(resolve => { finish = resolve; }));
    handlers.get('ui.changed')!();
    expect(store.trees.counter?.html).toBe(tree.html);
    finish({ nyaui: 1, html: '<main>new</main>' });
    await Promise.resolve();
    expect(store.trees.counter?.html).toBe('<main>new</main>');
  });

  it('rejects late stale responses and clears a denied page', async () => {
    const replies: ((value: unknown) => void)[] = [];
    state.session.request.mockImplementation(() => new Promise(resolve => replies.push(resolve)));
    const store = usePluginsStore();
    const first = store.loadTree('counter', true);
    const second = store.loadTree('counter', true);
    replies[1]({ nyaui: 1, html: 'new' }); await second;
    replies[0]({ nyaui: 1, html: 'old' }); await first;
    expect(store.trees.counter?.html).toBe('new');
    state.session.request.mockRejectedValueOnce(new Error('EACCES'));
    await expect(store.loadTree('counter', true)).rejects.toThrow('EACCES');
    expect(store.trees.counter).toBeUndefined();
  });
});
