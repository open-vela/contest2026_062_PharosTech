import { expect, it } from 'vitest';
import { mount } from '@vue/test-utils';
import { nextTick } from 'vue';
import NyaUiSurface from '../src/components/NyaUiSurface.vue';
import type { NyaUiTree } from '../src/types';

it('switches views in the host toolbar without invoking plugin commands', async () => {
  const tree: NyaUiTree = { nyaui: 1, views: {
    component: { nyaui: 1, root: { type: 'button', id: 'add', props: { text: '计数' }, on: { tap: { command: 'increment' } } } },
    html: { nyaui: 1, html: '<button>HTML counter</button>', events: { increment: { command: 'increment' } } },
  } };
  const wrapper = mount(NyaUiSurface, { props: { tree, pluginId: 'counter' } });
  const tabs = wrapper.findAll('[role=tab]');
  await tabs[1].trigger('click');
  expect(wrapper.find('iframe').exists()).toBe(true);
  expect(wrapper.emitted('command')).toBeUndefined();
  window.dispatchEvent(new MessageEvent('message', { source: wrapper.get('iframe').element.contentWindow, data: { nyaui: 1, type: 'event', event: 'increment' } }));
  await nextTick();
  expect(wrapper.emitted('command')?.[0][0]).toMatchObject({ view: 'html', event: 'increment' });
  await tabs[0].trigger('click');
  await wrapper.findAll('button').find(button => button.text() === '计数')!.trigger('click');
  expect(wrapper.emitted('command')?.[1][0]).toMatchObject({ view: 'component', componentId: 'add', command: 'increment' });
  wrapper.unmount();
});
