import { describe, expect, it } from 'vitest';
import { mount } from '@vue/test-utils';
import NyaUiPage from '../src/components/NyaUiPage.vue';
import NyaHtmlFrame from '../src/components/NyaHtmlFrame.vue';
import { getNyaComponent } from '../src/registry.js';
import { KIT_TYPES, registerKit } from '../src/kit.js';
import { buildHtmlSdk } from '../src/html-sdk.js';
import { validateTree } from '../src/validate.js';
import type { NyaUiCommand, NyaUiTree } from '../src/types.js';

describe('Feature Kit registration', () => {
  it('registers every kit type (mediaPlayer included) and is idempotent', () => {
    registerKit();
    registerKit();
    expect(getNyaComponent('mediaPlayer')).toBeDefined();
    expect(getNyaComponent('mediaPlayer')!.kind).toBe('display');
    for (const t of KIT_TYPES) expect(getNyaComponent(t), t).toBeDefined();
    expect(KIT_TYPES).toContain('notice');
    expect(KIT_TYPES).not.toContain('banner');
  });

  it('allows listSection children in validation', () => {
    const tree: NyaUiTree = {
      nyaui: 1,
      root: { type: 'listSection', props: { title: 'x' }, children: [{ type: 'text', props: { text: 'a' } }] },
    };
    expect(validateTree(tree)).toBeNull();
  });

  it('renders a toggleRow and emits change command with the new value', async () => {
    const tree: NyaUiTree = {
      nyaui: 1,
      root: {
        type: 'listSection',
        props: { title: '设置' },
        children: [
          {
            type: 'toggleRow',
            id: 'led',
            props: { title: '指示灯', icon: 'lightbulb', value: false },
            on: { change: { command: 'led.set', args: { $on: 'led' } } },
          },
        ],
      },
    };
    const w = mount(NyaUiPage, { props: { tree } });
    expect(w.text()).toContain('指示灯');
    const sw = w.find('.md-switch .track');
    expect(sw.exists()).toBe(true);
    await sw.trigger('click');
    const events = w.emitted('command')!;
    expect(events).toHaveLength(1);
    const payload = events[0][0] as NyaUiCommand;
    expect(payload.componentId).toBe('led');
    expect(payload.event).toBe('change');
    expect(payload.command).toBe('led.set');
    expect(payload.args).toEqual({ on: true });
    expect(payload.values).toEqual({ led: true });
    expect(w.find('.md-switch').classes()).toContain('on');
  });

  it('tile tap fires the tap command; contactList carries the contact id in args', async () => {
    const tree: NyaUiTree = {
      nyaui: 1,
      root: {
        type: 'column',
        children: [
          { type: 'tile', id: 't1', props: { title: '客厅灯', icon: 'lightbulb' }, on: { tap: { command: 'light.toggle' } } },
          {
            type: 'contactList',
            id: 'c',
            props: { contacts: [{ id: 'mom', name: '妈妈', actionIcon: 'call' }] },
            on: { action: { command: 'call.start' } },
          },
        ],
      },
    };
    const w = mount(NyaUiPage, { props: { tree } });
    await w.find('.nk-tile').trigger('click');
    await w.find('.nk-contact-action').trigger('click');
    const events = w.emitted('command')!.map((e) => e[0] as NyaUiCommand);
    expect(events[0]).toMatchObject({ componentId: 't1', event: 'tap', command: 'light.toggle' });
    expect(events[1]).toMatchObject({ componentId: 'c', event: 'action', command: 'call.start', args: { contact: 'mom' } });
  });
});

describe('NyaHtmlFrame', () => {
  it('builds a sandboxed srcdoc with sdk + theme and relays frame events as command', () => {
    const w = mount(NyaHtmlFrame, { props: { html: '<button id="b">go</button>', pluginId: 'demo' } });
    const iframe = w.find('iframe');
    expect(iframe.attributes('sandbox')).toBe('allow-scripts');
    const doc = iframe.attributes('srcdoc')!;
    expect(doc).toContain('window.nyabula');
    expect(doc).toContain('"demo"');
    expect(doc).toContain('<button id="b">go</button>');

    // Simulate a frame -> host event (source check needs the real contentWindow).
    const el = iframe.element as HTMLIFrameElement;
    window.dispatchEvent(
      new MessageEvent('message', {
        data: { nyaui: 1, type: 'event', componentId: 'b', event: 'tap', command: 'demo.go', values: { x: 1 } },
        source: el.contentWindow,
      }),
    );
    const cmds = w.emitted('command');
    expect(cmds).toBeTruthy();
    expect(cmds![0][0]).toEqual({ componentId: 'b', event: 'tap', command: 'demo.go', args: undefined, values: { x: 1 } });
  });

  it('buildHtmlSdk bakes pluginId and theme into the script', () => {
    const src = buildHtmlSdk('p1', { '--md-primary': '#62dfaf' });
    expect(src).toContain('pluginId: "p1"');
    expect(src).toContain('"--md-primary":"#62dfaf"');
    expect(src).not.toContain('__NYA_');
  });
});
