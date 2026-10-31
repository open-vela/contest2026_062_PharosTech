import { afterEach, expect, it, vi } from 'vitest';
import { mount } from '@vue/test-utils';
import NyaUiPage from '../src/components/NyaUiPage.vue';
import type { NyaUiTree } from '../src/types';

afterEach(() => vi.useRealTimers());

function form(change = false): NyaUiTree {
  return { nyaui: 1, root: { type: 'column', children: [
    { type: 'textField', id: 'amount', props: { label: 'Amount', value: '' },
      ...(change ? { on: { change: { command: 'preview' } } } : {}) },
    { type: 'button', id: 'save', props: { text: 'Save' }, on: { tap: { command: 'save' } } },
  ] } };
}

it('keeps unbound input edits local and sends them with the declared button', async () => {
  vi.useFakeTimers();
  const wrapper = mount(NyaUiPage, { props: { tree: form() } });
  await wrapper.get('input').setValue('8.80');
  await vi.advanceTimersByTimeAsync(500);
  expect(wrapper.emitted('command')).toBeUndefined();
  await wrapper.get('button').trigger('click');
  expect(wrapper.emitted('command')?.[0][0]).toMatchObject({ componentId: 'save',
    event: 'tap', command: 'save', values: { amount: '8.80' } });
  wrapper.unmount();
});

it('debounces declared changes and discards events from a replaced tree', async () => {
  vi.useFakeTimers();
  const wrapper = mount(NyaUiPage, { props: { tree: form(true) } });
  await wrapper.get('input').setValue('1');
  await wrapper.get('input').setValue('12');
  await vi.advanceTimersByTimeAsync(300);
  expect(wrapper.emitted('command')).toHaveLength(1);
  expect(wrapper.emitted('command')?.[0][0]).toMatchObject({ command: 'preview', values: { amount: '12' } });
  await wrapper.get('input').setValue('123');
  await wrapper.setProps({ tree: form() });
  await vi.advanceTimersByTimeAsync(500);
  expect(wrapper.emitted('command')).toHaveLength(1);
  wrapper.unmount();
});
