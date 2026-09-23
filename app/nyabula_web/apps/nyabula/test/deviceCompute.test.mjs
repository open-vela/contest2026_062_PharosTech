import test from 'node:test';
import assert from 'node:assert/strict';
import {
  BUSY_DELAY_MS, BUSY_RETRIES, LLM_REQUIRED_FILES, LOCAL_BACKEND_HOST, POLL_FAST_MS, POLL_SLOW_MS,
  blobEtaSeconds, blobPercent, capabilityLabel, errorCode, fmtEta, fmtRate, isLocalBackend, isTransient, lastRunText, llmStatusText,
  llmStatusTone, missingLlmFiles, onDeviceErrorText, parseComputeStatus, parseOnDevice, pollIntervalMs, retryOnBusy, transferText,
} from '../src/lib/deviceCompute.ts';
import { MODEL_CATALOGUE } from '../src/lib/deviceModels.ts';

const MB = 1024 * 1024;

/** What ny_compute.c answers while it copies the model. */
const provisioning = {
  running: true, linked: true, generation: 7, capabilities: ['health', 'llm', 'blob'], capabilityMask: 15,
  blob: { active: true, hashing: false, name: 'llm/model.rkllm', offset: 210 * MB, size: 840 * MB, bytesPerSec: 21 * MB },
  llm: { state: 'provisioning', model: 'llm/model.rkllm', promptTokens: 0, completionTokens: 0, prefillMs: 0, tokensPerSec: 0, lastError: null },
  lastError: null, generationChanges: 0, droppedFrames: 0,
};

function busyError(code = 'EBUSY') {
  return Object.assign(new Error(code), { code });
}

test('compute.status: the device payload comes through', () => {
  const s = parseComputeStatus(provisioning);
  assert.equal(s.running && s.linked, true);
  assert.equal(s.generation, 7);
  // The mask has the chat bit the list does not word yet.
  assert.deepEqual(s.capabilities, ['health', 'llm', 'blob', 'chat']);
  assert.deepEqual(s.blob, { active: true, hashing: false, name: 'llm/model.rkllm', offset: 210 * MB, size: 840 * MB, bytesPerSec: 21 * MB });
  assert.equal(s.llm.state, 'provisioning');
  assert.equal(s.llm.lastError, '');
  assert.equal(s.lastError, '');
});

test('compute.status: whatever is missing or wrong becomes a safe default', () => {
  for (const raw of [undefined, null, 'x', 42, [], {}, { blob: 'no', llm: [], capabilities: 'llm' }]) {
    const s = parseComputeStatus(raw);
    assert.deepEqual(
      [s.running, s.linked, s.generation, s.capabilities, s.capabilityMask, s.lastError, s.generationChanges, s.droppedFrames],
      [false, false, 0, [], 0, '', 0, 0],
    );
    assert.deepEqual(s.blob, { active: false, hashing: false, name: '', offset: 0, size: 0, bytesPerSec: 0 });
    assert.deepEqual(s.llm, { state: 'unknown', model: '', promptTokens: 0, completionTokens: 0, prefillMs: 0, tokensPerSec: 0, lastError: '' });
  }
  const odd = parseComputeStatus({
    running: 'yes', linked: 1, capabilityMask: -3, capabilities: ['npu', 7, '', 'llm', 'npu', 'x'.repeat(40)],
    blob: { active: true, offset: 900, size: 100, bytesPerSec: NaN, name: 5 },
    llm: { state: 'warming-up', tokensPerSec: '9', promptTokens: -1, lastError: '-2: no such file' },
    lastError: 17,
  });
  assert.equal(odd.running, false);
  assert.equal(odd.linked, false);
  // Known names first, then what this panel does not know yet, once each.
  assert.deepEqual(odd.capabilities, ['llm', 'npu']);
  assert.equal(odd.blob.offset, 100, 'an offset past the end is the end');
  assert.equal(odd.blob.bytesPerSec, 0);
  assert.equal(odd.blob.name, '');
  assert.equal(odd.llm.state, 'unknown');
  assert.equal(odd.llm.tokensPerSec, 0);
  assert.equal(odd.llm.promptTokens, 0);
  assert.equal(odd.llm.lastError, '-2: no such file');
  assert.equal(odd.lastError, '');
  for (const state of ['unloaded', 'provisioning', 'loading', 'ready', 'busy', 'error']) assert.equal(parseComputeStatus({ llm: { state } }).llm.state, state);
});

test('capabilities: the mask alone is enough, and names have words', () => {
  assert.deepEqual(parseComputeStatus({ capabilityMask: 5 }).capabilities, ['health', 'blob']);
  assert.deepEqual(parseComputeStatus({ capabilityMask: 8 }).capabilities, ['chat']);
  for (const name of ['health', 'llm', 'blob', 'chat']) assert.notEqual(capabilityLabel(name), name);
  assert.equal(capabilityLabel('npu'), 'npu');
});

test('agent.config.ondevice: parsed, and a taken slot is never "enabled"', () => {
  assert.deepEqual(
    parseOnDevice({ available: true, enabled: true, slot: 3, priority: 20, model: 'llm/model.rkllm', failures: 1, calls: 12, latencyMs: 8450.6 }),
    { available: true, enabled: true, slot: 3, priority: 20, model: 'llm/model.rkllm', failures: 1, calls: 12, latencyMs: 8451 },
  );
  const taken = parseOnDevice({ available: false, enabled: true, slot: 3 });
  assert.deepEqual([taken.available, taken.enabled], [false, false]);
  for (const raw of [undefined, null, [], 'x', {}]) {
    assert.deepEqual(parseOnDevice(raw), { available: true, enabled: false, slot: 0, priority: 0, model: '', failures: 0, calls: 0, latencyMs: 0 });
  }
  assert.equal(parseOnDevice({ priority: 500 }).priority, 100);
  assert.equal(parseOnDevice({ priority: -5, calls: '3', enabled: 'true' }).priority, 0);
  assert.equal(parseOnDevice({ calls: '3' }).calls, 0);
  assert.equal(parseOnDevice({ enabled: 'true' }).enabled, false);
});

test('the local backend is known by its host', () => {
  assert.equal(LOCAL_BACKEND_HOST, 'nyabula.local');
  assert.equal(isLocalBackend('nyabula.local'), true);
  assert.equal(isLocalBackend(' Nyabula.Local '), true);
  for (const other of ['api.openai.com', 'nyabula.local.evil.example', 'x.nyabula.local', '', null, undefined, 3]) assert.equal(isLocalBackend(other), false);
});

test('model files: both are needed, and an unfinished upload is not a file', () => {
  assert.deepEqual(LLM_REQUIRED_FILES.map((f) => f.path), ['llm/model.rkllm', 'llm/tokenizer.json']);
  assert.deepEqual(missingLlmFiles([]).map((f) => f.path), ['llm/model.rkllm', 'llm/tokenizer.json']);
  assert.deepEqual(missingLlmFiles([{ path: 'llm/model.rkllm', partial: false }]).map((f) => f.path), ['llm/tokenizer.json']);
  assert.deepEqual(missingLlmFiles([{ path: 'llm/model.rkllm', partial: true }, { path: 'llm/tokenizer.json', partial: false }]).map((f) => f.path), ['llm/model.rkllm']);
  assert.deepEqual(missingLlmFiles([{ path: 'llm/model.rkllm', partial: false }, { path: 'llm/tokenizer.json', partial: false }]), []);
  // The old place of the tokenizer does not satisfy the load.
  assert.equal(missingLlmFiles([{ path: 'llm/model.rkllm', partial: false }, { path: 'llm/tokenizer/tokenizer.json', partial: false }]).length, 1);
  // The models page asks for the same two files, as required ones.
  const llm = MODEL_CATALOGUE.find((k) => k.kind === 'llm');
  for (const f of LLM_REQUIRED_FILES) assert.equal(llm.files.find((x) => `llm/${x.name}` === f.path)?.required, true, f.path);
});

test('progress: percentage, rate and time left', () => {
  assert.equal(blobPercent({ offset: 210 * MB, size: 840 * MB }), 25);
  assert.equal(blobPercent({ offset: 839 * MB, size: 840 * MB }), 99, 'not 100 before the last byte');
  assert.equal(blobPercent({ offset: 840 * MB, size: 840 * MB }), 100);
  assert.equal(blobPercent({ offset: 900, size: 100 }), 100);
  for (const b of [{ offset: 5, size: 0 }, { offset: 0, size: 10 }, { offset: -1, size: 10 }, { offset: NaN, size: NaN }]) assert.equal(blobPercent(b), 0);

  assert.equal(fmtRate(21 * MB), '21 MB/s');
  assert.equal(fmtRate(12.34 * MB), '12.3 MB/s');
  assert.equal(fmtRate(150 * MB), '150 MB/s');
  assert.equal(fmtRate(900), '900 B/s');
  assert.equal(fmtRate(1536), '1.5 KB/s');
  for (const none of [0, -1, NaN, Infinity]) assert.equal(fmtRate(none), '');

  assert.equal(blobEtaSeconds({ offset: 210 * MB, size: 840 * MB, bytesPerSec: 21 * MB }), 30);
  assert.equal(blobEtaSeconds({ offset: 840 * MB, size: 840 * MB, bytesPerSec: 21 * MB }), 0);
  assert.equal(blobEtaSeconds({ offset: 0, size: 840 * MB, bytesPerSec: 0 }), -1);
  assert.equal(blobEtaSeconds({ offset: 0, size: 0, bytesPerSec: 5 }), -1);
  assert.equal(fmtEta(30), '约 30 秒');
  assert.equal(fmtEta(80), '约 1 分 20 秒');
  assert.equal(fmtEta(3700), '约 1 小时 1 分');
  assert.equal(fmtEta(0), '即将完成');
  for (const none of [-1, NaN, Infinity]) assert.equal(fmtEta(none), '');

  const blob = parseComputeStatus(provisioning).blob;
  assert.equal(transferText(blob, '正在搬运模型'), '正在搬运模型 25% · 21 MB/s · 剩余约 30 秒');
  assert.equal(transferText({ ...blob, bytesPerSec: 0 }, '正在搬运模型'), '正在搬运模型 25%');
  assert.equal(transferText({ ...blob, size: 0, offset: 0, bytesPerSec: 0 }, '正在搬运模型'), '正在搬运模型');
  assert.equal(transferText({ ...blob, offset: blob.size }, '正在搬运模型'), '正在搬运模型 100% · 21 MB/s · 即将完成');
});

test('the status line says each state in the words of the card', () => {
  const at = (llm, extra = {}) => parseComputeStatus({ ...provisioning, ...extra, llm: { ...provisioning.llm, ...llm } });
  assert.equal(llmStatusText(at({ state: 'unloaded' })), '未加载');
  assert.equal(llmStatusText(at({})), '正在搬运模型 25% · 21 MB/s · 剩余约 30 秒');
  assert.equal(llmStatusText(at({}, { blob: { active: false, hashing: true, name: 'llm/model.rkllm' } })), '正在核对模型文件…');
  assert.equal(llmStatusText(at({ state: 'loading' })), '正在加载');
  assert.equal(llmStatusText(at({ state: 'ready' })), '就绪');
  assert.equal(llmStatusText(at({ state: 'busy' })), '推理中');
  assert.equal(llmStatusText(at({ state: 'error', lastError: '-2: tokenizer.json missing' })), '出错: -2: tokenizer.json missing');
  assert.equal(llmStatusText(at({ state: 'error' }, { lastError: '-110: link lost' })), '出错: -110: link lost');
  assert.equal(llmStatusText(at({ state: 'error' })), '出错: 原因未知');
  assert.match(llmStatusText(at({ state: 'dreaming' })), /未知/);
  // Nothing the model says counts while the compute domain is away.
  assert.match(llmStatusText(at({ state: 'ready' }, { linked: false })), /尚未连上/);
  assert.match(llmStatusText(at({ state: 'ready' }, { linked: false, running: false })), /没有运行/);

  assert.deepEqual(['unloaded', 'provisioning', 'loading', 'ready', 'busy', 'error', 'unknown'].map((state) => llmStatusTone(at({ state }))),
    ['info', 'warn', 'warn', 'ok', 'ok', 'err', 'info']);
  assert.equal(llmStatusTone(at({ state: 'ready' }, { linked: false })), 'warn');
});

test('polling: every second only while something moves', () => {
  assert.deepEqual([POLL_FAST_MS, POLL_SLOW_MS], [1000, 10000]);
  for (const state of ['provisioning', 'loading', 'busy']) {
    assert.equal(isTransient(state), true, state);
    assert.equal(pollIntervalMs(state), 1000, state);
  }
  for (const state of ['unloaded', 'ready', 'error', 'unknown']) {
    assert.equal(isTransient(state), false, state);
    assert.equal(pollIntervalMs(state), 10000, state);
  }
  // A file being pulled in for another reason (an upload just finished) is worth watching too.
  assert.equal(pollIntervalMs('ready', true), 1000);
});

test('last run: nothing before the first one, then the four numbers', () => {
  assert.equal(lastRunText(parseComputeStatus({}).llm), '');
  assert.equal(
    lastRunText(parseComputeStatus({ llm: { promptTokens: 412, completionTokens: 36, prefillMs: 1830.4, tokensPerSec: 9.6 } }).llm),
    '提示 412 tokens · 回复 36 tokens · 预填充 1830 ms · 9.6 tokens/s',
  );
  assert.equal(lastRunText(parseComputeStatus({ llm: { promptTokens: 10, completionTokens: 2, tokensPerSec: 12 } }).llm), '提示 10 tokens · 回复 2 tokens · 12 tokens/s');
});

test('error codes are read from whatever was thrown', () => {
  assert.equal(errorCode(busyError()), 'EBUSY');
  assert.equal(errorCode({ code: 'ENOTFOUND' }), 'ENOTFOUND');
  for (const none of [undefined, null, 'EBUSY', new Error('x'), { code: 16 }, ['EBUSY']]) assert.equal(errorCode(none), '');
  assert.match(onDeviceErrorText(busyError('EEXIST')), /槽位/);
  assert.match(onDeviceErrorText(busyError('EBUSY')), /稍后/);
  assert.match(onDeviceErrorText(busyError('EACCES')), /主人/);
  assert.match(onDeviceErrorText(busyError('ENOENT')), /模型/);
  assert.equal(onDeviceErrorText(Object.assign(new Error('boom'), { code: 'EIO' })), 'boom');
  assert.equal(onDeviceErrorText('plain'), 'plain');
});

test('retryOnBusy: waits out EBUSY, and nothing else', async () => {
  assert.deepEqual([BUSY_RETRIES, BUSY_DELAY_MS], [5, 2000]);
  const sleeps = [];
  const sleep = async (ms) => { sleeps.push(ms); };

  // Answers at once: no wait.
  assert.equal(await retryOnBusy(async () => 'ok', { sleep }), 'ok');
  assert.deepEqual(sleeps, []);

  // Busy three times, then an answer.
  let calls = 0;
  const notes = [];
  const value = await retryOnBusy(async () => {
    if (++calls <= 3) throw busyError();
    return { enabled: true };
  }, { sleep, onRetry: (attempt, retries) => notes.push(`${attempt}/${retries}`) });
  assert.deepEqual(value, { enabled: true });
  assert.equal(calls, 4);
  assert.deepEqual(sleeps, [2000, 2000, 2000]);
  assert.deepEqual(notes, ['1/5', '2/5', '3/5']);

  // Always busy: one attempt and five retries, then the EBUSY itself.
  calls = 0;
  sleeps.length = 0;
  await assert.rejects(retryOnBusy(async () => { calls++; throw busyError(); }, { sleep }), { code: 'EBUSY' });
  assert.equal(calls, 6);
  assert.equal(sleeps.length, 5);

  // Another failure is not waited for.
  calls = 0;
  sleeps.length = 0;
  await assert.rejects(retryOnBusy(async () => { calls++; throw busyError('EEXIST'); }, { sleep }), { code: 'EEXIST' });
  await assert.rejects(retryOnBusy(async () => { calls++; throw busyError('ENOTFOUND'); }, { sleep }), { code: 'ENOTFOUND' });
  assert.equal(calls, 2);
  assert.deepEqual(sleeps, []);

  // Busy first, then a different failure: that one is reported.
  calls = 0;
  await assert.rejects(retryOnBusy(async () => { throw busyError(++calls === 1 ? 'EBUSY' : 'EACCES'); }, { sleep }), { code: 'EACCES' });
  assert.equal(calls, 2);

  // Own numbers.
  calls = 0;
  sleeps.length = 0;
  await assert.rejects(retryOnBusy(async () => { calls++; throw busyError(); }, { sleep, retries: 2, delayMs: 50 }), { code: 'EBUSY' });
  assert.equal(calls, 3);
  assert.deepEqual(sleeps, [50, 50]);
});

test('retryOnBusy: a view that went away stops asking', async () => {
  let calls = 0;
  let gone = false;
  const sleeps = [];
  // Gone before the wait: no wait, no second attempt.
  gone = true;
  await assert.rejects(retryOnBusy(async () => { calls++; throw busyError(); }, { sleep: async (ms) => { sleeps.push(ms); }, cancelled: () => gone }), { code: 'EBUSY' });
  assert.equal(calls, 1);
  assert.deepEqual(sleeps, []);
  // Gone during the wait: the attempt after it is not made.
  calls = 0;
  gone = false;
  await assert.rejects(retryOnBusy(async () => { calls++; throw busyError(); }, { sleep: async () => { gone = true; }, cancelled: () => gone }), { code: 'EBUSY' });
  assert.equal(calls, 1);
});
