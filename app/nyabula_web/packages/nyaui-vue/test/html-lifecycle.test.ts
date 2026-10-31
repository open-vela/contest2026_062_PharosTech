import { describe, expect, it } from 'vitest';
import { mount } from '@vue/test-utils';
import { nextTick } from 'vue';
import NyaHtmlFrame from '../src/components/NyaHtmlFrame.vue';
import NyaUiPage from '../src/components/NyaUiPage.vue';

describe('HTML page lifecycle', () => {
  it('grows and shrinks with content from its own frame without a fixed backdrop', async () => {
    const wrapper = mount(NyaHtmlFrame, { props: { html: '<main>Counter</main>', pluginId: 'counter' } });
    const frame = wrapper.get('iframe').element;
    const resize = (height: number, source: Window | null = frame.contentWindow) => window.dispatchEvent(new MessageEvent('message', { source, data: { nyaui: 1, type: 'resize', height } }));
    resize(510); await nextTick();
    expect(frame.style.height).toBe('510px');
    resize(180); await nextTick();
    expect(frame.style.height).toBe('180px');
    resize(900, window); await nextTick();
    expect(frame.style.height).toBe('180px');
    resize(Number.NaN); await nextTick();
    expect(frame.style.height).toBe('180px');
    expect(frame.getAttribute('srcdoc')).toContain('background:transparent');
    expect(frame.getAttribute('srcdoc')).toContain('color-scheme:normal');
    wrapper.unmount();
  });

  it('preserves an explicitly requested frame height', async () => {
    const wrapper = mount(NyaHtmlFrame, { props: { html: '<main>Counter</main>', pluginId: 'counter', height: 240 } });
    const frame = wrapper.get('iframe').element;
    window.dispatchEvent(new MessageEvent('message', { source: frame.contentWindow, data: { nyaui: 1, type: 'resize', height: 510 } }));
    await nextTick();
    expect(frame.style.height).toBe('240px');
    wrapper.unmount();
  });

  it('does not navigate the iframe when the host theme changes', async () => {
    const wrapper = mount(NyaHtmlFrame, { props: { html: '<main>Counter</main>', pluginId: 'counter' } });
    await nextTick();
    const frame = wrapper.get('iframe').element;
    const source = frame.getAttribute('srcdoc');
    document.documentElement.style.setProperty('--md-primary', '#123456');
    document.documentElement.dataset.theme = 'light';
    await new Promise(resolve => setTimeout(resolve, 0));
    await nextTick();
    expect(wrapper.get('iframe').element).toBe(frame);
    expect(frame.getAttribute('srcdoc')).toBe(source);
    expect(source).toContain('<main>Counter</main>');
    wrapper.unmount();
  });

  it('keeps the frame mounted when an identical full tree arrives again', async () => {
    const tree = { nyaui: 1, html: '<button>Back</button>' };
    const wrapper = mount(NyaUiPage, { props: { tree, pluginId: 'counter' } });
    const frame = wrapper.get('iframe').element;
    for (let i = 0; i < 5; i++) await wrapper.setProps({ tree: { ...tree } });
    expect(wrapper.get('iframe').element).toBe(frame);
    expect(frame.getAttribute('sandbox')).toBe('allow-scripts');
    wrapper.unmount();
  });
});
