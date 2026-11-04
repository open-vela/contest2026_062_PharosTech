import { expect, it } from 'vitest';
import { createApp, h } from 'vue';
import { lightingArtwork } from '../src/lib/lightingArtwork';
import LightingPreview from '../src/views/services/features/LightingPreview.vue';

it('maps all 33 actual pixels to two ears, a vertical chest strip and the base', () => {
  const layers = lightingArtwork(Array(33).fill(0));
  expect(layers.map(layer => layer.pixel)).toEqual(Array.from({ length: 33 }, (_, i) => i));
  expect(layers.filter(layer => layer.zone === 'chest')).toHaveLength(8);
  expect(layers.filter(layer => layer.zone === 'base')).toHaveLength(23);
  expect(layers[2].y).toBeLessThan(layers[9].y);
  expect(layers[10].x).toBeLessThan(layers[32].x);
  expect(layers.every(layer => layer.glow === 'none')).toBe(true);
});
it('does not light other zones when one real pixel changes', () => {
  const pixels = Array(33).fill(0); pixels[3] = 0xff0000;
  const layers = lightingArtwork(pixels, ['#6fd3ff', '#ffb86f']);
  expect(layers.filter(layer => layer.glow !== 'none').map(layer => layer.pixel)).toEqual([3]);
  const values = layers[3].matrix.split(' ').map(Number);
  expect([values[0], values[6], values[12]]).toEqual([1, 0, 0]);
  expect(layers.slice(-2).map(layer => layer.id)).toEqual(['eye-0', 'eye-1']);
  expect(layers.at(-2)?.matrix).not.toBe(layers.at(-1)?.matrix);
});
it('keeps body and light PNGs separate, with distinct SVG IDs for full and compact previews', () => {
  const host = document.createElement('div'); document.body.append(host);
  const app = createApp({ render: () => h('div', [h(LightingPreview, { pixels: Array(33).fill(0xff0000) }), h(LightingPreview, { compact: true, pixels: Array(33).fill(0x0000ff) })]) });
  app.mount(host);
  try {
    const ids = [...host.querySelectorAll('[id]')].map(el => el.id);
    expect(new Set(ids).size).toBe(ids.length);
    expect(host.querySelectorAll('.robot-body')).toHaveLength(2);
    expect(host.querySelector('image.robot-body')?.getAttribute('href')).toContain('nyabula-front.png');
    expect(host.querySelector('g[data-zone="left_ear"] image')?.getAttribute('href')).toContain('nyabula-light.png');
  } finally { app.unmount(); host.remove(); }
});
