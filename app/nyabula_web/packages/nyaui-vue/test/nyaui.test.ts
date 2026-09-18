import { describe, expect, it } from 'vitest';
import { mount } from '@vue/test-utils';
import NyaUiPage from '../src/components/NyaUiPage.vue';
import { applyPatches } from '../src/patch.js';
import { validateTree } from '../src/validate.js';
import { collectValues, resolveArgs } from '../src/values.js';
import type { NyaUiCommand, NyaUiNode, NyaUiTree } from '../src/types.js';
import { feederTree } from './feeder.js';

function mountPage(tree: NyaUiTree) {
  return mount(NyaUiPage, { props: { tree } });
}

describe('NyaUiPage rendering', () => {
  it('renders the feeder demo tree without crashing', () => {
    const w = mountPage(feederTree());
    const html = w.html();
    expect(html).toContain('立即喂食');
    expect(html).toContain('余粮');
    expect(html).toContain('出粮克数');
    expect(w.findAll('svg').length).toBeGreaterThan(0); // chart + icons
    expect(w.find('.nyaui-error').exists()).toBe(false);
    expect(html).toMatchSnapshot();
  });

  it('renders unknown types as gray placeholders', () => {
    const tree: NyaUiTree = {
      nyaui: 1,
      root: { type: 'column', children: [{ type: 'hologram3d' }] },
    };
    const w = mountPage(tree);
    const ph = w.find('.nya-unknown');
    expect(ph.exists()).toBe(true);
    expect(ph.text()).toBe('hologram3d');
  });

  it('emits command with resolved $args and values snapshot on button tap', async () => {
    const w = mountPage(feederTree());
    const btn = w.findAll('button').find((b) => b.text().includes('立即喂食'))!;
    await btn.trigger('click');
    const events = w.emitted('command')!;
    expect(events).toHaveLength(1);
    const payload = events[0][0] as NyaUiCommand;
    expect(payload.componentId).toBe('feed');
    expect(payload.event).toBe('tap');
    expect(payload.command).toBe('feed.now');
    expect(payload.args).toEqual({ grams: 20 });
    expect(payload.values).toEqual({ amount: 20, sched: true, times: ['07:00', '19:00'] });
  });

  it('switch toggles optimistically and fires change immediately', async () => {
    const w = mountPage(feederTree());
    await w.find('.nya-switch-track').trigger('click');
    const payload = (w.emitted('command')![0][0]) as NyaUiCommand;
    expect(payload.componentId).toBe('sched');
    expect(payload.event).toBe('change');
    expect(payload.command).toBe('feed.schedule.toggle');
    expect(payload.values.sched).toBe(false); // optimistic new value in snapshot
    expect(w.find('.nya-switch-track').classes()).not.toContain('on');
  });
});

describe('limit validation', () => {
  it('rejects trees deeper than 12', () => {
    let node: NyaUiNode = { type: 'text', props: { text: 'x' } };
    for (let i = 0; i < 13; i++) node = { type: 'column', children: [node] };
    const tree: NyaUiTree = { nyaui: 1, root: node };
    expect(validateTree(tree)).toMatch(/depth/);
    const w = mountPage(tree);
    expect(w.find('.nyaui-error').exists()).toBe(true);
  });

  it('rejects trees with more than 200 nodes', () => {
    const kids: NyaUiNode[] = Array.from({ length: 220 }, () => ({ type: 'spacer' }));
    const tree: NyaUiTree = { nyaui: 1, root: { type: 'column', children: kids } };
    expect(validateTree(tree)).toMatch(/node count/);
  });

  it('rejects over-long strings and children on non-containers', () => {
    const long: NyaUiTree = {
      nyaui: 1,
      root: { type: 'text', props: { text: 'a'.repeat(5000) } },
    };
    expect(validateTree(long)).toMatch(/string/);
    const badKids: NyaUiTree = {
      nyaui: 1,
      root: { type: 'text', props: { text: 'x' }, children: [{ type: 'spacer' }] },
    };
    expect(validateTree(badKids)).toMatch(/children/);
  });
});

describe('applyPatches', () => {
  it('rejects prototype paths and inherited traversal', () => {
    const tree = feederTree();
    for (const path of ['/__proto__/nyauiProbe', '/constructor/prototype/nyauiProbe', '/root/props/__proto__']) {
      try {
        expect(applyPatches(tree, [{path, value: 'unsafe'}])).toBe(false);
        expect(({} as Record<string, unknown>).nyauiProbe).toBeUndefined();
      } finally {
        delete (Object.prototype as Record<string, unknown>).nyauiProbe;
      }
    }
  });
  it('applies JSON Pointer patches in place', () => {
    const tree = feederTree();
    const ok = applyPatches(tree, [
      { path: '/root/children/0/children/0/props/value', value: '65' },
      { path: '/root/children/1/children/0/props/value', value: 35 },
    ]);
    expect(ok).toBe(true);
    expect(tree.root.children![0].children![0].props!.value).toBe('65');
    expect(tree.root.children![1].children![0].props!.value).toBe(35);
  });

  it('returns false when a path does not resolve', () => {
    const tree = feederTree();
    expect(applyPatches(tree, [{ path: '/root/children/99/props/value', value: 1 }])).toBe(false);
    expect(applyPatches(tree, [{ path: '/root/nope/deep', value: 1 }])).toBe(false);
    expect(applyPatches(tree, [{ path: 'no-leading-slash', value: 1 }])).toBe(false);
  });
});

describe('values helpers', () => {
  it('collects input values with overrides', () => {
    const tree = feederTree();
    const values = collectValues(tree.root, { amount: 45, notAnInput: 1 });
    expect(values).toEqual({ amount: 45, sched: true, times: ['07:00', '19:00'] });
  });

  it('resolves $refs and passes literals through', () => {
    expect(resolveArgs({ $grams: 'amount', mode: 'fast' }, { amount: 20 })).toEqual({
      grams: 20,
      mode: 'fast',
    });
    expect(resolveArgs(undefined, {})).toBeUndefined();
  });
});
