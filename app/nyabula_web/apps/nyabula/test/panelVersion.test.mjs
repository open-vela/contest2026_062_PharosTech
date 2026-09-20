import test from 'node:test';
import assert from 'node:assert/strict';
import { entryOf, isStale, runningEntry } from '../src/lib/panelVersion.ts';

const html = name => `<!doctype html><script type="module" crossorigin src="./assets/${name}"></script>`;

test('the entry script is read from index.html', () => {
  assert.equal(entryOf(html('app-DSOSAUm9.js')), 'app-DSOSAUm9.js');
  assert.equal(entryOf('<html></html>'), null);
});
test('the running entry comes from the document scripts', () => {
  assert.equal(runningEntry([{ src: '' }, { src: 'http://10.0.0.2/assets/app-CV2svys8.js' }]), 'app-CV2svys8.js');
  assert.equal(runningEntry([{ src: 'http://10.0.0.2/other.js' }]), null);
});
test('stale only when both are known and differ', () => {
  assert.equal(isStale('app-a.js', 'app-b.js'), true);
  assert.equal(isStale('app-a.js', 'app-a.js'), false);
  assert.equal(isStale(null, 'app-b.js'), false);
  assert.equal(isStale('app-a.js', null), false);
});
