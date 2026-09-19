import test from 'node:test';
import assert from 'node:assert/strict';
import { cleanLogText, fmtBytes, isUnsupportedError, mergeLogLines, parseCloudStatus, parseLogsTail, parseStorageStatus, parseUpdateStatus } from '../src/lib/deviceMaint.ts';

test('byte formatting', () => {
  assert.deepEqual([0, 512, 1024, 1536, 150 * 1024 * 1024, 29.5 * 1024 ** 3].map(fmtBytes), ['0 B', '512 B', '1 KB', '1.5 KB', '150 MB', '29.5 GB']);
  assert.equal(fmtBytes(NaN), '0 B');
});

test('ENOTFOUND = topic unknown to this firmware', () => {
  assert.equal(isUnsupportedError(Object.assign(new Error('x'), { code: 'ENOTFOUND' })), true);
  for (const e of [Object.assign(new Error('x'), { code: 'EPERM' }), null, 'ENOTFOUND']) assert.equal(isUnsupportedError(e), false);
});

test('storage.status: volumes, usage, freed', () => {
  const s = parseStorageStatus({
    volumes: [{ id: 'data', label: '数据分区', path: '/data', total: 1000, used: 250, free: 750 }, { path: '/tmp', total: 100, free: 40 }, 'junk'],
    usage: [{ id: 'logs', label: '日志', path: '/data/logs', bytes: 42, clearable: true }, { id: 'sys', bytes: 7 }, { label: 'no id' }],
    freed: 12,
  });
  assert.deepEqual(s.volumes[0], { id: 'data', label: '数据分区', path: '/data', total: 1000, used: 250, free: 750, percent: 25 });
  assert.deepEqual(s.volumes[1], { id: '/tmp', label: '/tmp', path: '/tmp', total: 100, used: 60, free: 40, percent: 60 });
  assert.deepEqual(s.usage, [
    { id: 'logs', label: '日志', path: '/data/logs', bytes: 42, clearable: true },
    { id: 'sys', label: 'sys', path: '', bytes: 7, clearable: false },
  ]);
  assert.equal(s.freed, 12);
  assert.deepEqual(parseStorageStatus(null), { volumes: [], usage: [], freed: null });
});

test('update.status: slots and current', () => {
  const u = parseUpdateStatus({
    current: { version: '0.4.1', builtAt: '2026-09-18 21:04:11', slot: 'b' },
    slots: [
      { name: 'a', active: false, bootable: true, successful: true, triesRemaining: 0 },
      { name: 'b', active: true, bootable: true, successful: false, triesRemaining: 3 },
      { name: 'c' },
    ],
    channel: 'manual', online: false, detail: 'OTA package or USB',
  });
  assert.deepEqual(u.current, { version: '0.4.1', builtAt: '2026-09-18 21:04:11', slot: 'b' });
  assert.deepEqual(u.slots.map((s) => [s.name, s.active, s.triesRemaining]), [['a', false, 0], ['b', true, 3]]);
  assert.equal(u.online, false);
  assert.deepEqual(parseUpdateStatus(undefined), { current: { version: '', builtAt: '', slot: '' }, slots: [], channel: 'manual', online: false, detail: '' });
});

test('logs.tail: parse, ANSI stripping, cursor fallback', () => {
  const esc = String.fromCharCode(27);
  assert.equal(cleanLogText(`${esc}[31mERR${esc}[0m\tboom` + String.fromCharCode(7)), 'ERR\tboom');
  const t = parseLogsTail({ lines: [{ seq: 5, text: 'a' }, { seq: 'x', text: 'bad' }, { seq: 6, text: 'b' }], next: 7, dropped: false });
  assert.deepEqual(t, { lines: [{ seq: 5, text: 'a' }, { seq: 6, text: 'b' }], next: 7, dropped: false });
  assert.equal(parseLogsTail({ lines: [{ seq: 9, text: '' }] }).next, 9);
  assert.deepEqual(parseLogsTail(null), { lines: [], next: null, dropped: false });
});

test('log view: append, cap, gap marker, restart', () => {
  const tail = (from, n, dropped = false) => ({ lines: Array.from({ length: n }, (_, i) => ({ seq: from + i, text: `l${from + i}` })), next: from + n, dropped });
  let view = mergeLogLines([], tail(1, 3), 5);
  assert.deepEqual(view.map((l) => l.seq), [1, 2, 3]);
  view = mergeLogLines(view, tail(2, 3), 5); // overlap: 2 and 3 are not repeated
  assert.deepEqual(view.map((l) => l.seq), [1, 2, 3, 4]);
  assert.equal(mergeLogLines(view, tail(1, 2), 5), view); // nothing new: same array
  view = mergeLogLines(view, tail(10, 2, true), 5); // gap marker + cap at 5
  assert.deepEqual(view.map((l) => [l.seq, l.mark === true]), [[3, false], [4, false], [9.5, true], [10, false], [11, false]]);
  view = mergeLogLines(view, tail(1, 2), 5, true); // device rebooted
  assert.deepEqual(view.map((l) => [l.seq, l.mark === true]), [[0.5, true], [1, false], [2, false]]);
});

test('cloud.status: device contract and the earlier shape', () => {
  assert.deepEqual(parseCloudStatus({ enabled: true, url: 'wss://c', state: 'unsupported', detail: 'relay client not built' }),
    { enabled: true, url: 'wss://c', state: 'unsupported', detail: 'relay client not built', deviceId: null, claimCode: null });
  assert.equal(parseCloudStatus({ enabled: true, url: 'wss://c', connected: true }).state, 'online');
  assert.equal(parseCloudStatus({ enabled: true, connected: false }).state, 'offline');
  assert.equal(parseCloudStatus({ enabled: false, connected: true }).state, 'disabled');
  assert.equal(parseCloudStatus({ enabled: true, state: 'bogus' }).state, 'offline');
  assert.deepEqual(parseCloudStatus({ enabled: true, connected: true, deviceId: 'nya-1', claimCode: 'ABC123' }).claimCode, 'ABC123');
  assert.equal(parseCloudStatus(null).state, 'disabled');
});
