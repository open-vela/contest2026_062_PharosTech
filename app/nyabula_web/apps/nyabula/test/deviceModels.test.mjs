import test from 'node:test';
import assert from 'node:assert/strict';
import {
  MODEL_CATALOGUE, destinationFor, fmtDuration, groupModels, isModelPath, kindSpec, modelsEndpoint, modelsUploadUrl, parseModelsList,
  parseModelsStatus, pushSample, safeSegment, sizeRefusal, transferRate, verifyFailureText,
} from '../src/lib/deviceModels.ts';

const sha = 'a'.repeat(64);

test('catalogue: every expected name is a path the device accepts', () => {
  assert.deepEqual(MODEL_CATALOGUE.map((k) => k.kind), ['llm', 'asr', 'tts', 'kws', 'speaker', 'face']);
  for (const k of MODEL_CATALOGUE) {
    assert.ok(k.purpose.length > 0 && k.files.some((f) => f.required), k.kind);
    for (const f of k.files) assert.equal(isModelPath(`${k.kind}/${f.name}`), true, `${k.kind}/${f.name}`);
  }
  assert.equal(kindSpec('llm')?.files[0]?.name, 'model.rkllm');
  assert.equal(kindSpec('nope'), null);
});

test('path rule mirrors the device', () => {
  for (const ok of ['llm/model.rkllm', 'tts/dict/jieba.dict.utf8', 'asr/a/b/c.onnx', 'face/_x-1.onnx', 'llm/tokenizer/tokenizer.json']) assert.equal(isModelPath(ok), true, ok);
  for (const bad of [
    '', 'llm', 'llm/', 'other/x.bin', 'llm/../x', 'llm/.hidden', 'llm/x.', 'llm/a b', 'llm/a/b/c/d.bin', '/llm/x', 'llm//x', 'LLM/x.bin',
    'llm/x.part', 'llm/x.PART', 'llm/x.part.json', 'llm/x.sha256', 'llm/中文.bin', `llm/${'a'.repeat(49)}`, `llm/${'a'.repeat(48)}/${'b'.repeat(48)}`,
  ]) assert.equal(isModelPath(bad), false, bad);
  // A model's own json is an ordinary file; only this module's sidecars are reserved.
  assert.equal(isModelPath('llm/config.json'), true);
});

test('safeSegment: whatever the file was called becomes one accepted name', () => {
  assert.equal(safeSegment('C:\\models\\My Model (v2).rkllm'), 'My_Model_v2_.rkllm');
  assert.equal(safeSegment('.hidden'), 'hidden');
  assert.equal(safeSegment('模型.onnx'), '_.onnx');
  assert.equal(safeSegment('...'), 'file');
  assert.equal(safeSegment('x.part'), 'x.part_');
  const long = safeSegment(`${'n'.repeat(80)}.onnx`);
  assert.equal(long.length, 48);
  assert.ok(long.endsWith('.onnx'));
  for (const name of ['a b.bin', 'x.part', 'x.sha256', `${'q'.repeat(90)}.rknn`, '中文名字.txt']) assert.equal(isModelPath(`llm/${safeSegment(name)}`), true, name);
});

test('destination: expected name for the name itself or a known alias, own name otherwise', () => {
  assert.deepEqual(destinationFor('asr', 'encoder-epoch-99-avg-1.int8.onnx'), { path: 'asr/encoder.onnx', spec: kindSpec('asr').files[0], renamed: true });
  assert.equal(destinationFor('asr', 'tokens.txt').path, 'asr/tokens.txt');
  assert.equal(destinationFor('asr', 'tokens.txt').renamed, false);
  assert.equal(destinationFor('asr', 'TOKENS.TXT').path, 'asr/tokens.txt');
  assert.equal(destinationFor('llm', 'MiniCPM5-1B-w4a16.rkllm').path, 'llm/model.rkllm');
  assert.equal(destinationFor('llm', 'tokenizer.json').path, 'llm/tokenizer/tokenizer.json');
  assert.equal(destinationFor('tts', 'jieba.dict.utf8').path, 'tts/dict/jieba.dict.utf8');
  assert.equal(destinationFor('tts', 'vocoder128.rknn').path, 'tts/vocoder.rknn');
  assert.equal(destinationFor('face', 'face_recognition_sface_2021dec.onnx').path, 'face/sface.onnx');
  const other = destinationFor('tts', 'my notes.md');
  assert.deepEqual(other, { path: 'tts/my_notes.md', spec: null, renamed: true });
  // Two files that fit the same loose alias: the second keeps its own name.
  const taken = new Set(['model.onnx']);
  assert.equal(destinationFor('speaker', 'a.onnx').path, 'speaker/model.onnx');
  assert.equal(destinationFor('speaker', 'b.onnx', taken).path, 'speaker/b.onnx');
  // An exact name always wins, taken or not: it is a replacement.
  assert.equal(destinationFor('speaker', 'model.onnx', taken).path, 'speaker/model.onnx');
});

test('models.list: items, limits, junk', () => {
  const l = parseModelsList({
    root: '/data/models', free: 1000, volume: 4000, margin: 64, fileLimit: 2147483647, pieceLimit: 8388608, kinds: ['llm', 7], truncated: true,
    items: [
      { path: 'llm/model.rkllm', kind: 'llm', bytes: 875760324, mtime: 1790000000000, partial: false, sha256: sha.toUpperCase() },
      { path: 'asr/encoder.onnx', kind: 'asr', bytes: 100, mtime: 0, partial: true, received: 100, total: 300, sha256: sha },
      { path: 'asr/encoder.onnx', kind: 'asr', bytes: 5, partial: true },
      { path: 'tts/x.bin', bytes: 1, sha256: 'short' },
      { bytes: 3 }, 'junk',
    ],
  });
  assert.equal(l.items.length, 3);
  assert.deepEqual(l.items[0], { path: 'llm/model.rkllm', kind: 'llm', bytes: 875760324, mtime: 1790000000000, partial: false, received: 0, total: 0, sha256: sha });
  assert.deepEqual(l.items[1], { path: 'asr/encoder.onnx', kind: 'asr', bytes: 100, mtime: 0, partial: true, received: 100, total: 300, sha256: sha });
  assert.deepEqual({ kind: l.items[2].kind, sha256: l.items[2].sha256 }, { kind: 'tts', sha256: '' });
  assert.deepEqual([l.free, l.volume, l.margin, l.fileLimit, l.kinds, l.truncated, l.removed], [1000, 4000, 64, 2147483647, ['llm'], true, null]);
  const empty = parseModelsList(null);
  assert.deepEqual([empty.root, empty.items, empty.margin, empty.fileLimit, empty.pieceLimit], ['/data/models', [], 64 * 1024 * 1024, 0xffffffff, 8 * 1024 * 1024]);
  assert.equal(parseModelsList({ removed: false }).removed, false);
});

test('groups: expected files first, extras after, readiness from required files', () => {
  const l = parseModelsList({
    items: [
      { path: 'asr/encoder.onnx', kind: 'asr', bytes: 10, partial: false },
      { path: 'asr/decoder.onnx', kind: 'asr', bytes: 10, partial: false },
      { path: 'asr/joiner.onnx', kind: 'asr', bytes: 10, partial: true, received: 4, total: 10, sha256: sha },
      { path: 'asr/zzz.bin', kind: 'asr', bytes: 1, partial: false },
      { path: 'face/sface.onnx', kind: 'face', bytes: 7, partial: false },
    ],
  });
  const groups = groupModels(l);
  const asr = groups.find((g) => g.spec.kind === 'asr');
  assert.deepEqual(asr.rows.map((r) => [r.name, r.state]), [['encoder.onnx', 'present'], ['decoder.onnx', 'present'], ['joiner.onnx', 'partial'], ['tokens.txt', 'missing'], ['zzz.bin', 'present']]);
  assert.deepEqual([asr.ready, asr.missingRequired, asr.bytes], [false, 2, 31]);
  assert.equal(asr.rows[4].spec, null);
  assert.equal(groups.find((g) => g.spec.kind === 'face').ready, true);
  assert.equal(groups.find((g) => g.spec.kind === 'llm').rows.every((r) => r.state === 'missing'), true);
  assert.equal(groupModels(null).length, 6);
});

test('size refusal says why', () => {
  const l = parseModelsList({ free: 200 * 1024 * 1024, margin: 64 * 1024 * 1024, fileLimit: 0xffffffff });
  assert.equal(sizeRefusal(l, 100 * 1024 * 1024), '');
  assert.match(sizeRefusal(l, 150 * 1024 * 1024), /剩余空间不足/);
  // What the device already holds of it does not have to fit a second time.
  assert.equal(sizeRefusal(l, 150 * 1024 * 1024, 100 * 1024 * 1024), '');
  assert.match(sizeRefusal(l, 2 ** 32), /4 GB/);
  assert.match(sizeRefusal(parseModelsList({ free: 2 ** 40, fileLimit: 2147483647 }), 3 * 2 ** 30), /2 GB/);
  assert.match(sizeRefusal(l, 0), /空/);
  assert.equal(sizeRefusal(null, 5), '');
});

test('models.status', () => {
  const s = parseModelsStatus({
    busy: true,
    verify: { path: 'llm/model.rkllm', state: 'running', done: 5, total: 10, sha256: '', expected: '', reason: '' },
    upload: { active: true, path: 'asr/encoder.onnx', phase: 'hashing', received: 9, total: 9, hashed: 3 },
  });
  assert.deepEqual(s.verify, { path: 'llm/model.rkllm', state: 'running', done: 5, total: 10, sha256: '', expected: '', reason: '' });
  assert.deepEqual(s.upload, { active: true, path: 'asr/encoder.onnx', phase: 'hashing', received: 9, total: 9, hashed: 3 });
  const idle = parseModelsStatus({ verify: { state: 'weird' } });
  assert.deepEqual([idle.busy, idle.verify.state, idle.upload.active, idle.upload.phase], [false, 'idle', false, 'receiving']);
  assert.match(verifyFailureText('mismatch'), /不一致/);
  assert.equal(verifyFailureText('???'), '校验失败');
});

test('endpoint: same origin only, path escaped', () => {
  const loc = { protocol: 'http:', host: '192.168.4.1' };
  assert.deepEqual(modelsEndpoint('self', loc), { url: 'http://192.168.4.1/models/upload', tokenSlot: 'nyalink.token:ws://192.168.4.1/nyalink' });
  assert.equal(modelsEndpoint('lan:192.168.4.1', loc)?.url, 'http://192.168.4.1/models/upload');
  assert.equal(modelsEndpoint('lan:10.0.0.2', loc), null);
  assert.equal(modelsEndpoint(null, loc), null);
  assert.equal(modelsEndpoint('self', { protocol: 'file:', host: '' }), null);
  assert.equal(modelsUploadUrl('http://h/models/upload', 'tts/dict/jieba.dict.utf8'), 'http://h/models/upload?path=tts%2Fdict%2Fjieba.dict.utf8');
});

test('speed and time left', () => {
  assert.deepEqual([45, 200, 3900, -1, NaN].map(fmtDuration), ['45 秒', '3 分 20 秒', '1 小时 5 分', '', '']);
  let s = [];
  s = pushSample(s, 0, 0);
  assert.equal(transferRate(s), 0);
  s = pushSample(s, 1000, 2_000_000);
  s = pushSample(s, 2000, 4_000_000);
  assert.equal(transferRate(s), 2_000_000);
  // Old samples leave the window.
  s = pushSample(s, 20000, 40_000_000);
  assert.equal(s.length, 1);
  // A piece that is sent again makes the counter go back: start over rather than report a negative speed.
  s = pushSample(pushSample([], 0, 5000), 100, 1000);
  assert.deepEqual(s, [{ at: 100, bytes: 1000 }]);
});
