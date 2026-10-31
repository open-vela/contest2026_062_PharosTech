import test from 'node:test';
import assert from 'node:assert/strict';
import { keyboardInset } from '../src/lib/keyboardInset.ts';

test('keyboard inset = layout viewport minus what the visual viewport shows', () => {
  assert.equal(keyboardInset(800, 800, 0), 0); // no keyboard
  assert.equal(keyboardInset(800, 480, 0), 320); // keyboard open
  assert.equal(keyboardInset(800, 480, 40), 280); // page panned up by the browser
  assert.equal(keyboardInset(800, 479.6, 0), 320); // fractional viewport
});

test('browser chrome and zoom rounding are not a keyboard', () => {
  assert.equal(keyboardInset(800, 760, 0), 0);
  assert.equal(keyboardInset(800, 740, 0), 0);
  assert.equal(keyboardInset(800, 739, 0), 61);
  assert.equal(keyboardInset(800, 900, 0), 0); // pinch-zoomed out: never negative
});
