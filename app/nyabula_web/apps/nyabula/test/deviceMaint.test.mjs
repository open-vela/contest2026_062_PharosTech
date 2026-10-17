import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import {
  appliedTarget, applyErrorText, applyNotice, applyRefusalText, applyRequest, cleanLogText, findTarget, fmtBytes, formatHint, imageFormatOk,
  isFirmwareImage, isMountedRefusal, isUnsupportedError, mergeLogLines, needsAmpConfirm, needsConfirm, otaEndpoint, otaUploadUrl, parseCloudStatus,
  parseLogsTail, parseStorageStatus, parseUpdateStatus, parseUploadReply, pendingReboot, shortDigest, unavailableText, unlockMatches, unlockPhrase,
  uploadErrorText,
} from '../src/lib/deviceMaint.ts';
import { createSha256, sha256Hex } from '../src/lib/sha256.ts';

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
      { name: 'a', active: false, running: false, bootable: true, successful: true, priority: 14, version: 3, size: 4096, triesRemaining: 0 },
      { name: 'b', active: true, running: true, bootable: true, successful: false, priority: 15, version: 4, size: 8192 },
      { name: 'c' },
    ],
    channel: 'upload', online: false, upload: true, maxBytes: 64 * 1024 * 1024, target: 'a',
    apply: { state: 'failed', error: 74, reason: 'verify' },
    detail: 'written to the slot that is not running',
  });
  assert.deepEqual(u.current, { version: '0.4.1', builtAt: '2026-09-18 21:04:11', slot: 'b' });
  assert.deepEqual(u.slots, [
    { name: 'a', active: false, running: false, bootable: true, successful: true, priority: 14, version: 3, size: 4096 },
    { name: 'b', active: true, running: true, bootable: true, successful: false, priority: 15, version: 4, size: 8192 },
  ]);
  assert.equal('triesRemaining' in u.slots[0], false);
  assert.deepEqual([u.online, u.upload, u.maxBytes, u.target, u.channel], [false, true, 64 * 1024 * 1024, 'a', 'upload']);
  assert.deepEqual(u.apply, { state: 'failed', error: 74, reason: 'verify', target: '', forced: false });
  assert.equal(applyErrorText(u.apply), '写入后从存储回读的内容不一致（错误码 74）');
  assert.equal(needsConfirm(u), true);
  assert.equal(pendingReboot(u), '');
  assert.deepEqual(parseUpdateStatus(undefined), {
    current: { version: '', builtAt: '', slot: '' }, slots: [], ampSlots: [], ampActive: false, targets: [], channel: 'manual', online: false,
    detail: '', upload: false, maxBytes: 0, target: '', apply: { state: 'idle', error: 0, reason: '', target: '', forced: false },
  });
});

test('update.status: older shape, staged update, unknown apply state', () => {
  // No `running`, no `target`: both follow from the current slot.
  const old = parseUpdateStatus({ current: { slot: 'a' }, slots: [{ name: 'a', active: true, bootable: true, successful: true }, { name: 'b', bootable: true }], upload: true, maxBytes: 100 });
  assert.deepEqual(old.slots.map((s) => [s.name, s.running]), [['a', true], ['b', false]]);
  assert.equal(old.target, 'b');
  assert.equal(needsConfirm(old), false);
  // Firmware without the upload path: nothing to write to, whatever else it says.
  const none = parseUpdateStatus({ current: { slot: 'a' }, slots: [{ name: 'a', active: true, running: true, bootable: true }], maxBytes: 100 });
  assert.deepEqual([none.upload, none.maxBytes, none.target, needsConfirm(none)], [false, 0, '', false]);
  // After update.apply: B starts next, A still runs.
  const staged = parseUpdateStatus({
    current: { slot: 'a' }, upload: true, apply: { state: 'done', error: 0 },
    slots: [{ name: 'a', active: false, running: true, bootable: true, successful: true }, { name: 'b', active: true, running: false, bootable: true }],
  });
  assert.equal(pendingReboot(staged), 'b');
  assert.equal(staged.apply.state, 'done');
  // While writing, the target is not bootable yet: no reboot on offer.
  const writing = parseUpdateStatus({ current: { slot: 'a' }, upload: true, apply: { state: 'writing' }, slots: [{ name: 'a', active: true, running: true, bootable: true }, { name: 'b', bootable: false }] });
  assert.equal(pendingReboot(writing), '');
  assert.equal(parseUpdateStatus({ apply: { state: 'exploded' } }).apply.state, 'idle');
});

test('update.status: targets, AMP slots, what an apply left behind', () => {
  const MiB = 1024 * 1024;
  const u = parseUpdateStatus({
    current: { slot: 'a' }, upload: true, maxBytes: 64 * MiB, target: 'b',
    slots: [{ name: 'a', active: true, running: true, bootable: true, successful: true, size: 9 }, { name: 'b' }],
    ampSlots: [{ name: 'a', bootable: false }, { name: 'b', active: true, bootable: true, successful: false, priority: 15, version: 2, size: 40 * MiB }],
    ampActive: true, ampTarget: 'a', stagingFree: 300 * MiB,
    targets: [
      { id: 'nuttx', label: 'openvela 固件', description: 'd', kind: 'slot', format: 'arm64', advanced: false, maxBytes: 64 * MiB, capacity: 64 * MiB, slot: 'b', available: true, reason: '', mounted: false },
      { id: 'amp', label: 'AMP 计算域镜像', kind: 'slot', format: 'fit', advanced: false, maxBytes: 300 * MiB, capacity: 512 * MiB, slot: 'a', available: true },
      { id: 'nboot', label: 'N-Boot', kind: 'nboot', format: 'fit', advanced: true, maxBytes: 4 * MiB, capacity: 4 * MiB },
      { id: 'partition:nuttx_a', kind: 'partition', format: 'arm64', advanced: true, maxBytes: 64 * MiB, available: false, reason: 'running' },
      { id: 'partition:config', kind: 'partition', format: 'fat', advanced: true, maxBytes: 32 * MiB, mounted: true },
      // A target this panel has never heard of, claiming to be harmless.
      { id: 'partition:mystery', kind: 'teleport', format: 'hologram', advanced: false, maxBytes: 1 },
      { id: 'nuttx', label: 'duplicate' }, { label: 'no id' }, 'junk',
    ],
    apply: { state: 'done', error: 0, reason: '', target: 'amp', forced: false },
  });
  assert.deepEqual(u.targets.map((t) => t.id), ['nuttx', 'amp', 'nboot', 'partition:nuttx_a', 'partition:config', 'partition:mystery']);
  assert.deepEqual(u.targets.map((t) => t.advanced), [false, false, true, true, true, true]);
  assert.deepEqual(findTarget(u, 'amp'), {
    id: 'amp', label: 'AMP 计算域镜像', description: '', kind: 'slot', format: 'fit', advanced: false, maxBytes: 300 * MiB, capacity: 512 * MiB,
    slot: 'a', available: true, reason: '', mounted: false,
  });
  const mystery = findTarget(u, 'partition:mystery');
  assert.deepEqual([mystery.kind, mystery.format, mystery.label, mystery.capacity], ['partition', 'raw', 'partition:mystery', 1]);
  assert.equal(findTarget(u, 'partition:data'), null);
  assert.equal(findTarget(null, 'nuttx'), null);
  assert.match(unavailableText(findTarget(u, 'partition:nuttx_a')), /正在运行/);
  assert.equal(unavailableText(findTarget(u, 'amp')), '');
  assert.equal(findTarget(u, 'partition:config').mounted, true);

  // AMP slots never run as seen from here, whatever the current slot is.
  assert.deepEqual(u.ampSlots.map((s) => [s.name, s.running, s.active]), [['a', false, false], ['b', false, true]]);
  assert.equal(u.ampActive, true);
  assert.equal(needsAmpConfirm(u), true);
  assert.equal(needsAmpConfirm(parseUpdateStatus({ upload: true, ampSlots: [{ name: 'a', active: true, bootable: true, successful: true, size: 1 }] })), false);
  // An active slot that was never filled has nothing to vouch for.
  assert.equal(needsAmpConfirm(parseUpdateStatus({ upload: true, ampSlots: [{ name: 'a', active: true, bootable: true }] })), false);

  // The AMP write shows in no NuttX slot: the apply record is what says "reboot".
  assert.equal(pendingReboot(u), '');
  assert.equal(appliedTarget(u).id, 'amp');
  assert.equal(appliedTarget(parseUpdateStatus({ upload: true, apply: { state: 'done', target: 'nuttx' } })), null);
  assert.equal(appliedTarget(parseUpdateStatus({ upload: true, targets: [{ id: 'amp' }], apply: { state: 'failed', target: 'amp' } })), null);

  // Firmware from before there were targets: the one it has, from what it said.
  const old = parseUpdateStatus({ current: { slot: 'b' }, upload: true, maxBytes: 100 });
  assert.deepEqual(old.targets, [{
    id: 'nuttx', label: 'openvela 固件', description: '', kind: 'slot', format: 'arm64', advanced: false, maxBytes: 100, capacity: 100, slot: 'a',
    available: true, reason: '', mounted: false,
  }]);
  assert.deepEqual(parseUpdateStatus({ targets: [{ id: 'nuttx' }] }).targets, []);
});

test('update.apply: request, refusals, forced notice', () => {
  const sha = 'cd'.repeat(32);
  const nuttx = { id: 'nuttx', advanced: false };
  const config = { id: 'partition:config', advanced: true };
  assert.deepEqual(applyRequest(nuttx, sha, false), { sha256: sha, target: 'nuttx' });
  assert.deepEqual(applyRequest(config, sha, false), { sha256: sha, target: 'partition:config', advanced: true });
  assert.deepEqual(applyRequest(config, sha, true), { sha256: sha, target: 'partition:config', advanced: true, force: true });

  const err = (code) => Object.assign(new Error(code), { code });
  for (const code of ['EADVANCED', 'ERUNNING', 'EMOUNTED', 'EBLOCKED', 'ENOTIMAGE', 'ETOOLARGE', 'ENOTCONFIGURED', 'EUNAVAILABLE', 'EBUSY']) {
    assert.notEqual(applyRefusalText(err(code)), '', code);
  }
  // Not one of its own: the caller decides (ENOTFOUND stays "unsupported").
  for (const e of [err('ENOTFOUND'), err('EINVAL'), null, 'EMOUNTED']) assert.equal(applyRefusalText(e), '');
  assert.equal(isMountedRefusal(err('EMOUNTED')), true);
  assert.equal(isMountedRefusal(err('EBUSY')), false);
  assert.equal(isUnsupportedError(err('ENOTFOUND')), true);

  assert.equal(applyNotice({ started: true, notice: '写完立即重启' }), '写完立即重启');
  assert.equal(applyNotice({ started: true }), '');
  assert.equal(applyNotice(null), '');
  assert.match(applyErrorText({ state: 'failed', error: 8, reason: 'format', target: 'amp', forced: false }), /不是这个目标需要的镜像（错误码 8）/);
});

test('advanced gate: the device name has to be typed', () => {
  assert.equal(unlockPhrase('  客厅的 Nya '), '客厅的 Nya');
  for (const none of ['', '   ', null, undefined]) assert.equal(unlockPhrase(none), '我已了解风险');
  assert.equal(unlockMatches(' 客厅的 Nya ', '客厅的 Nya'), true);
  assert.equal(unlockMatches('客厅的 nya', '客厅的 Nya'), false);
  assert.equal(unlockMatches('', '客厅的 Nya'), false);
  assert.equal(unlockMatches(null, '客厅的 Nya'), false);
  assert.equal(unlockMatches('', ''), false);
});

test('ota: per-target url, magic and digest display', () => {
  const url = 'http://192.168.4.1/ota/upload';
  assert.equal(otaUploadUrl(url, 'nuttx'), url);
  assert.equal(otaUploadUrl(url, ''), url);
  assert.equal(otaUploadUrl(url, 'amp'), `${url}?target=amp`);
  assert.equal(otaUploadUrl(url, 'partition:trust'), `${url}?target=partition%3Atrust`);

  const head = (at, bytes) => {
    const h = new Uint8Array(512);
    h.set(bytes, at);
    return h;
  };
  const arm = head(56, [0x41, 0x52, 0x4d, 0x64]);
  const fit = head(0, [0xd0, 0x0d, 0xfe, 0xed]);
  const ctrl = head(0, Array.from('K7ABCTRL', (c) => c.charCodeAt(0)));
  const fat = head(510, [0x55, 0xaa]);
  const formats = ['arm64', 'fit', 'bootctrl', 'fat'];
  [arm, fit, ctrl, fat].forEach((h, i) => {
    // Each header passes as its own format and as no other.
    assert.deepEqual(formats.map((f) => imageFormatOk(f, h)), formats.map((_, j) => i === j), formats[i]);
    assert.equal(imageFormatOk('raw', h), true);
  });
  assert.equal(imageFormatOk('fat', fat.subarray(0, 511)), false);
  assert.equal(imageFormatOk('fit', fit.subarray(0, 3)), false);
  assert.equal(imageFormatOk('raw', new Uint8Array(0)), false);
  for (const f of [...formats, 'raw']) assert.notEqual(formatHint(f), '');

  const sha = '9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08';
  assert.equal(shortDigest(sha), '9f86d081…b0f00a08');
  assert.equal(shortDigest(''), '');

  const reply = (status, error) => uploadErrorText(status, parseUploadReply(status, JSON.stringify({ error })));
  assert.match(reply(400, 'ETARGET'), /不认识/);
  assert.match(reply(403, 'EBLOCKED'), /不能从面板写入/);
  assert.match(reply(413, 'ETOOLARGE'), /比目标分区大/);
  assert.match(reply(413, 'ENOSPACE'), /剩余空间不足/);
  assert.match(reply(415, 'ENOTIMAGE'), /不是所选目标需要的镜像/);
});

test('ota: endpoint, image magic, upload replies', () => {
  assert.deepEqual(otaEndpoint('self', { protocol: 'http:', host: '192.168.4.1' }), { url: 'http://192.168.4.1/ota/upload', tokenSlot: 'nyalink.token:ws://192.168.4.1/nyalink' });
  assert.deepEqual(otaEndpoint('self', { protocol: 'https:', host: 'nya.local:8443' }), { url: 'https://nya.local:8443/ota/upload', tokenSlot: 'nyalink.token:wss://nya.local:8443/nyalink' });
  assert.equal(otaEndpoint('lan:10.0.0.9:7788', { protocol: 'http:', host: 'localhost:5173' }), null);
  assert.notEqual(otaEndpoint('lan:10.0.0.9:7788', { protocol: 'http:', host: '10.0.0.9:7788' }), null);
  for (const key of [null, 'cloud:abc', 'dev:preview']) assert.equal(otaEndpoint(key, { protocol: 'http:', host: 'x' }), null);
  assert.equal(otaEndpoint('self', { protocol: 'file:', host: '' }), null);

  const image = new Uint8Array(64);
  image.set([0x41, 0x52, 0x4d, 0x64], 56);
  assert.equal(isFirmwareImage(image), true);
  assert.equal(isFirmwareImage(image.subarray(0, 59)), false);
  assert.equal(isFirmwareImage(new Uint8Array(64)), false);

  const sha = 'ab'.repeat(32);
  assert.deepEqual(parseUploadReply(200, JSON.stringify({ received: 1234, sha256: sha.toUpperCase() })), { ok: true, received: 1234, sha256: sha, error: '' });
  const mismatch = parseUploadReply(422, JSON.stringify({ error: 'EDIGEST', received: 9, sha256: sha }));
  assert.deepEqual([mismatch.ok, mismatch.error], [false, 'EDIGEST']);
  assert.match(uploadErrorText(422, mismatch), /不一致/);
  // A 200 that is the page itself (firmware without /ota/) must not pass.
  const page = parseUploadReply(200, '<!doctype html><html></html>');
  assert.deepEqual([page.ok, page.error], [false, 'ENOTJSON']);
  assert.equal(uploadErrorText(200, page), '此固件不支持网页上传');
  assert.equal(parseUploadReply(200, JSON.stringify({ received: 1 })).ok, false);
  assert.match(uploadErrorText(0, parseUploadReply(0, '')), /连接中断/);
  assert.equal(uploadErrorText(500, parseUploadReply(500, '{"error":"ESTORAGE"}')), '上传失败（HTTP 500 ESTORAGE）');
});

test('sha256 fallback: vectors, block boundaries, chunked input', () => {
  const ascii = (t) => new TextEncoder().encode(t);
  assert.equal(sha256Hex(ascii('')), 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
  assert.equal(sha256Hex(ascii('abc')), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  assert.equal(sha256Hex(ascii('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq')), '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1');
  // Every length around the padding edges (55/56/63/64/65 ...), against node.
  let seed = 12345;
  const bytes = new Uint8Array(70000);
  for (let i = 0; i < bytes.length; i++) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    bytes[i] = seed >>> 16;
  }
  const want = (data) => createHash('sha256').update(data).digest('hex');
  for (const n of [1, 54, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 1000, 65536, 70000]) {
    assert.equal(sha256Hex(bytes.subarray(0, n)), want(bytes.subarray(0, n)), `length ${n}`);
  }
  // Pieces of awkward sizes must give what one piece gives.
  for (const step of [1, 7, 63, 64, 65, 1000, 4096]) {
    const h = createSha256();
    for (let at = 0; at < 10000; at += step) h.update(bytes.subarray(at, Math.min(10000, at + step)));
    assert.equal(h.hex(), want(bytes.subarray(0, 10000)), `step ${step}`);
  }
  const done = createSha256().update(ascii('abc'));
  assert.equal(done.hex(), done.hex());
  assert.throws(() => done.update(ascii('x')));
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
