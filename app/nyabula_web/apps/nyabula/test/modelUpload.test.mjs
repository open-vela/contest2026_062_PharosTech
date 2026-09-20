import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import {
  PIECE_BYTES, alreadyPresent, backoffMs, contentRange, digestCacheKey, hashSource, parsePieceReply, replyErrorText, resumeOffset, uploadFile, verdictOf,
} from '../src/lib/modelUpload.ts';
import { createSha256 } from '../src/lib/sha256.ts';

const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

function bytes(n, seed = 7) {
  const out = new Uint8Array(n);
  let x = seed;
  for (let i = 0; i < n; i++) {
    x = (x * 1103515245 + 12345) & 0x7fffffff;
    out[i] = x >>> 16;
  }
  return out;
}

/** The device side of the contract, as ny_web_models.c implements it: one
 *  part per path, a piece only where the part ends, hash at the end. */
function fakeDevice() {
  const dev = {
    parts: new Map(), files: new Map(), puts: [], gets: 0, sleeps: [],
    /** Queue of faults for the next PUTs: 'drop' (keeps half a piece), 'busy', 'lost-reply', 'corrupt'. */
    faults: [],
    reply(status, body) { return parsePieceReply(status, JSON.stringify(body)); },
    state(path) {
      dev.gets++;
      const p = dev.parts.get(path);
      const f = dev.files.get(path);
      return dev.reply(200, { path, received: p?.data.length ?? 0, total: p?.total ?? 0, sha256: p?.sha256 ?? '', exists: !!f, bytes: f?.length ?? 0, fileSha256: f ? sha256(f) : '' });
    },
    async put(path, piece, start, total, digest, onSent) {
      const data = piece ? new Uint8Array(await piece.arrayBuffer()) : new Uint8Array(0);
      dev.puts.push({ start, length: data.length });
      const fault = dev.faults.shift();
      if (fault === 'busy') return dev.reply(409, { error: 'EBUSY' });
      let p = dev.parts.get(path);
      if (!p || p.total !== total || p.sha256 !== digest) p = { total, sha256: digest, data: new Uint8Array(0), fresh: true };
      const probe = piece === null;
      if (probe ? p.data.length !== total : start !== p.data.length) return dev.reply(409, { error: 'EOFFSET', received: p.data.length, total });
      let take = data;
      if (fault === 'drop') take = data.subarray(0, data.length >> 1);
      if (fault === 'corrupt') take = data.map((b, i) => (i === 0 ? b ^ 1 : b));
      const next = new Uint8Array(p.data.length + take.length);
      next.set(p.data);
      next.set(take, p.data.length);
      p.data = next;
      dev.parts.set(path, p);
      onSent(take.length);
      if (fault === 'drop') return parsePieceReply(0, '');
      if (p.data.length === total) {
        if (fault === 'lost-reply') return parsePieceReply(0, '');
        return dev.finish(path, p);
      }
      return dev.reply(200, { path, received: p.data.length, total, complete: false });
    },
    finish(path, p) {
      const actual = sha256(p.data);
      dev.parts.delete(path);
      if (actual !== p.sha256) return dev.reply(422, { error: 'EDIGEST', received: p.data.length, sha256: actual });
      dev.files.set(path, p.data);
      return dev.reply(200, { path, received: p.total, total: p.total, complete: true, sha256: actual });
    },
  };
  /* A lost reply leaves a complete part behind; the piece-less request finishes it. */
  const put = dev.put;
  dev.put = async (path, piece, start, total, digest, onSent) => {
    const p = dev.parts.get(path);
    if (piece === null && p && p.data.length === total && p.total === total && p.sha256 === digest && !dev.faults.length) {
      dev.puts.push({ start, length: 0 });
      return dev.finish(path, p);
    }
    return put(path, piece, start, total, digest, onSent);
  };
  dev.io = {
    getState: async (path) => dev.state(path),
    putPiece: (path, piece, start, total, digest, onSent) => dev.put(path, piece, start, total, digest, onSent),
    sleep: async (ms) => { dev.sleeps.push(ms); },
  };
  return dev;
}

function run(dev, data, extra = {}) {
  const progress = [];
  const blob = new Blob([data]);
  return uploadFile({
    path: 'llm/model.rkllm', source: blob, sha256: sha256(data), io: dev.io, stopped: () => false,
    onProgress: (p) => progress.push(p), pieceBytes: 1000, ...extra,
  }).then((outcome) => ({ outcome, progress }));
}

test('reply parsing and what an answer means', () => {
  const r = parsePieceReply(409, '{"error":"EOFFSET","received":4096,"total":9000}');
  assert.deepEqual([r.json, r.error, r.received, r.total, verdictOf(r)], [true, 'EOFFSET', 4096, 9000, 'resync']);
  assert.equal(verdictOf(parsePieceReply(409, '{"error":"EBUSY"}')), 'retry');
  assert.equal(verdictOf(parsePieceReply(0, '')), 'retry');
  assert.equal(verdictOf(parsePieceReply(408, '{"error":"ETIMEDOUT","received":5}')), 'retry');
  assert.equal(verdictOf(parsePieceReply(500, '{"error":"ESTORAGE"}')), 'retry');
  assert.equal(verdictOf(parsePieceReply(200, '{"received":1,"complete":false}')), 'advance');
  for (const [status, body] of [[401, '{"error":"EAUTH"}'], [413, '{"error":"ENOSPACE"}'], [422, '{"error":"EDIGEST"}'], [507, '{"error":"ENOSPACE"}'], [404, '{"error":"ENOTFOUND"}'], [200, '<!doctype html>']]) {
    assert.equal(verdictOf(parsePieceReply(status, body)), 'fatal', String(status));
  }
  assert.equal(parsePieceReply(200, '[1]').json, false);
  assert.equal(parsePieceReply(200, '{"sha256":"ABCD","received":-4}').sha256, 'abcd');
  assert.equal(parsePieceReply(200, '{"received":-4}').received, 0);
});

test('error texts: unsupported firmware, the two size limits, space', () => {
  assert.equal(replyErrorText(parsePieceReply(200, '<!doctype html>')), '此固件不支持模型上传');
  assert.equal(replyErrorText(parsePieceReply(405, '')), '此固件不支持模型上传');
  assert.equal(replyErrorText(parsePieceReply(404, '{"error":"ENOTFOUND"}')), '此固件不支持模型上传');
  assert.match(replyErrorText(parsePieceReply(413, '{"error":"ETOOLARGE","reason":"fat32-file-limit"}')), /4 GB/);
  assert.match(replyErrorText(parsePieceReply(413, '{"error":"ETOOLARGE","reason":"offset-width"}')), /2 GB/);
  assert.match(replyErrorText(parsePieceReply(413, '{"error":"ENOSPACE","free":1,"needed":2}')), /空间不足/);
  assert.match(replyErrorText(parsePieceReply(507, '')), /空间不足/);
  assert.match(replyErrorText(parsePieceReply(401, '')), /重新用密码登录/);
  assert.match(replyErrorText(parsePieceReply(418, '{"error":"ETEAPOT"}')), /HTTP 418 ETEAPOT/);
});

test('ranges, resume offset, backoff', () => {
  assert.equal(contentRange(0, 8388608, 875760324), 'bytes 0-8388607/875760324');
  assert.equal(contentRange(875000000, 760324, 875760324), 'bytes 875000000-875760323/875760324');
  assert.equal(contentRange(10, 0, 10), 'bytes */10');
  const sha = 'b'.repeat(64);
  const state = (o) => parsePieceReply(200, JSON.stringify(o));
  assert.equal(resumeOffset(state({ received: 500, total: 900, sha256: sha }), 900, sha), 500);
  // Another file under the same name, or the same name with another size: from the start.
  assert.equal(resumeOffset(state({ received: 500, total: 900, sha256: 'c'.repeat(64) }), 900, sha), 0);
  assert.equal(resumeOffset(state({ received: 500, total: 901, sha256: sha }), 900, sha), 0);
  assert.equal(resumeOffset(parsePieceReply(500, '{}'), 900, sha), 0);
  assert.equal(alreadyPresent(state({ exists: true, fileSha256: sha }), sha), true);
  assert.equal(alreadyPresent(state({ exists: true, fileSha256: '' }), sha), false);
  assert.deepEqual([1, 2, 3, 4, 5, 6, 7, 50].map(backoffMs), [1000, 2000, 4000, 8000, 15000, 30000, 30000, 30000]);
  assert.equal(PIECE_BYTES, 8 * 1024 * 1024);
});

test('plain upload: sequential pieces, exact ranges, done on the device digest', async () => {
  const dev = fakeDevice();
  const data = bytes(3500);
  const { outcome, progress } = await run(dev, data);
  assert.deepEqual(outcome, { kind: 'done', sha256: sha256(data), skipped: false });
  assert.deepEqual(dev.puts, [{ start: 0, length: 1000 }, { start: 1000, length: 1000 }, { start: 2000, length: 1000 }, { start: 3000, length: 500 }]);
  assert.deepEqual(Buffer.from(dev.files.get('llm/model.rkllm')), Buffer.from(data));
  assert.equal(dev.gets, 1);
  assert.equal(progress[0].phase, 'checking');
  assert.equal(progress.at(-1).phase, 'finishing');
  assert.equal(progress.at(-1).confirmed, 3500);
  // Never reports going backwards while nothing failed.
  const sent = progress.map((p) => p.sent);
  assert.deepEqual(sent, [...sent].sort((a, b) => a - b));
});

test('resume after a reload: the GET says where to go on', async () => {
  const dev = fakeDevice();
  const data = bytes(5000);
  dev.parts.set('llm/model.rkllm', { total: 5000, sha256: sha256(data), data: data.slice(0, 2300) });
  const { outcome } = await run(dev, data);
  assert.equal(outcome.kind, 'done');
  assert.deepEqual(dev.puts[0], { start: 2300, length: 1000 });
  assert.deepEqual(Buffer.from(dev.files.get('llm/model.rkllm')), Buffer.from(data));
});

test('a part of another file under the same name is not resumed', async () => {
  const dev = fakeDevice();
  const data = bytes(2500);
  dev.parts.set('llm/model.rkllm', { total: 2500, sha256: 'd'.repeat(64), data: bytes(1200, 3) });
  const { outcome } = await run(dev, data);
  assert.equal(outcome.kind, 'done');
  assert.deepEqual(dev.puts[0], { start: 0, length: 1000 });
  assert.deepEqual(Buffer.from(dev.files.get('llm/model.rkllm')), Buffer.from(data));
});

test('the device already has exactly this file: nothing is sent', async () => {
  const dev = fakeDevice();
  const data = bytes(1800);
  dev.files.set('llm/model.rkllm', data);
  const { outcome } = await run(dev, data);
  assert.deepEqual(outcome, { kind: 'done', sha256: sha256(data), skipped: true });
  assert.equal(dev.puts.length, 0);
});

test('network errors: what was kept of a broken piece is resumed, with backoff', async () => {
  const dev = fakeDevice();
  const data = bytes(4200);
  dev.faults = [undefined, 'drop', 'busy', undefined, 'drop'];
  const { outcome, progress } = await run(dev, data);
  assert.equal(outcome.kind, 'done');
  assert.deepEqual(Buffer.from(dev.files.get('llm/model.rkllm')), Buffer.from(data));
  // Piece 2 broke after 500 bytes: the next one starts at 1500, not at 1000 and not at 2000.
  assert.deepEqual(dev.puts.slice(0, 4), [{ start: 0, length: 1000 }, { start: 1000, length: 1000 }, { start: 1500, length: 1000 }, { start: 1500, length: 1000 }]);
  // drop (1s), then busy (2s, second failure in a row), progress resets the count, drop again (1s).
  assert.deepEqual(dev.sleeps, [1000, 2000, 1000]);
  assert.ok(progress.some((p) => p.phase === 'waiting' && p.note.includes('连接中断')));
  assert.ok(progress.some((p) => p.phase === 'waiting' && p.note.includes('另一个文件')));
});

test('409 EOFFSET: the client goes where the device stands', async () => {
  const dev = fakeDevice();
  const data = bytes(3000);
  // The part grows behind the client's back (another tab), after the GET.
  const get = dev.io.getState;
  dev.io.getState = async (path) => {
    const reply = await get(path);
    dev.parts.set(path, { total: 3000, sha256: sha256(data), data: data.slice(0, 1700) });
    return reply;
  };
  const { outcome } = await run(dev, data);
  assert.equal(outcome.kind, 'done');
  assert.deepEqual(dev.puts.slice(0, 2), [{ start: 0, length: 1000 }, { start: 1700, length: 1000 }]);
  assert.deepEqual(dev.sleeps, []);
});

test('the reply to the last piece is lost: a piece-less request finishes the part', async () => {
  const dev = fakeDevice();
  const data = bytes(2000);
  dev.faults = [undefined, 'lost-reply'];
  const { outcome } = await run(dev, data);
  assert.deepEqual(outcome, { kind: 'done', sha256: sha256(data), skipped: false });
  assert.deepEqual(dev.puts.at(-1), { start: 2000, length: 0 });
  assert.deepEqual(Buffer.from(dev.files.get('llm/model.rkllm')), Buffer.from(data));
});

test('digest mismatch ends the upload and starts from zero next time', async () => {
  const dev = fakeDevice();
  const data = bytes(1500);
  dev.faults = ['corrupt'];
  const { outcome } = await run(dev, data);
  assert.equal(outcome.kind, 'failed');
  assert.match(outcome.text, /不一致/);
  assert.equal(outcome.confirmed, 0);
  assert.equal(dev.files.size, 0);
  const again = await run(dev, data);
  assert.equal(again.outcome.kind, 'done');
});

test('refusals are final, and a firmware without the endpoint says so', async () => {
  const dev = fakeDevice();
  dev.io.putPiece = async () => parsePieceReply(413, '{"error":"ENOSPACE","free":1,"needed":2,"margin":3}');
  const full = await run(dev, bytes(10));
  assert.equal(full.outcome.kind, 'failed');
  assert.match(full.outcome.text, /空间不足/);
  assert.deepEqual(dev.sleeps, []);

  const old = fakeDevice();
  old.io.getState = async () => parsePieceReply(200, '<!doctype html><title>Nyabula</title>');
  const res = await run(old, bytes(10));
  assert.deepEqual([res.outcome.kind, res.outcome.text], ['failed', '此固件不支持模型上传']);
});

test('pause: the loop ends between steps and says how far it got', async () => {
  const dev = fakeDevice();
  const data = bytes(5000);
  let stop = false;
  const put = dev.io.putPiece;
  dev.io.putPiece = async (...args) => {
    const reply = await put(...args);
    if (dev.puts.length === 2) stop = true;
    return reply;
  };
  const { outcome } = await run(dev, data, { stopped: () => stop });
  assert.equal(outcome.kind, 'stopped');
  assert.equal(dev.puts.length, 2);
  // Going on later resumes from what the device holds.
  stop = false;
  dev.io.putPiece = put;
  const resumed = await run(dev, data);
  assert.equal(resumed.outcome.kind, 'done');
  assert.deepEqual(dev.puts[2], { start: 2000, length: 1000 });
});

test('maxAttempts bounds a link that never comes back', async () => {
  const dev = fakeDevice();
  dev.io.putPiece = async () => parsePieceReply(0, '');
  const { outcome } = await run(dev, bytes(100), { maxAttempts: 3 });
  assert.equal(outcome.kind, 'failed');
  assert.match(outcome.text, /多次重试/);
  assert.deepEqual(dev.sleeps, [1000, 2000]);
});

test('hashing in slices equals hashing at once, and can be stopped', async () => {
  const data = bytes(1_000_003);
  const blob = new Blob([data]);
  const seen = [];
  const hex = await hashSource(blob, createSha256(), (b) => b.arrayBuffer(), (n) => seen.push(n), () => false, 65537);
  assert.equal(hex, sha256(data));
  assert.equal(seen.at(-1), data.length);
  assert.equal(seen.length, Math.ceil(data.length / 65537));
  // Known vector through the same path: one million 'a'.
  const million = new Blob([new Uint8Array(1_000_000).fill(0x61)]);
  assert.equal(await hashSource(million, createSha256(), (b) => b.arrayBuffer(), () => undefined, () => false, 99_991), 'cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0');
  assert.equal(await hashSource(new Blob([]), createSha256(), (b) => b.arrayBuffer(), () => undefined, () => false), 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
  let calls = 0;
  assert.equal(await hashSource(blob, createSha256(), (b) => b.arrayBuffer(), () => undefined, () => ++calls > 3, 65537), null);
  assert.equal(digestCacheKey({ name: 'm.rkllm', size: 5, lastModified: 9 }), 'm.rkllm|5|9');
});
