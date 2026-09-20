import test from 'node:test';
import assert from 'node:assert/strict';
import {
  INPUT_ROUTES, OUTPUT_ROUTES, activeOutputLabel, audioSummary, inputRouteLabel, isAudioUnsupported, micGainDb, outputRouteLabel,
  parseAudioStatus, sceneAudioRoute,
} from '../src/lib/audioControl.ts';
import { audioScene, sanitizeScenePayload } from '../src/composables/eyeScenePayload.ts';

/** What audio.status answers on the board with nothing plugged in. */
const wire = () => ({
  available: true, inputAvailable: true, restored: true, settingsSaved: true, revision: 7,
  output: { route: 'auto', activeRoute: 'speaker', headphones: false, volume: 40, muted: false, mono: false, swap: false, invertLeft: false, invertRight: false },
  input: { route: 'main', muted: false, gain: 100, gainDb: 24 },
});

test('mic gain: nearest of the nine 3 dB steps, same arithmetic as the firmware', () => {
  assert.deepEqual([0, 6, 7, 12, 13, 50, 93, 94, 100].map(micGainDb), [0, 0, 3, 3, 3, 12, 21, 24, 24]);
  for (let gain = 0; gain <= 100; gain++) {
    const db = micGainDb(gain);
    assert.ok(db >= 0 && db <= 24 && db % 3 === 0, `gain ${gain} -> ${db}`);
    if (gain > 0) assert.ok(db >= micGainDb(gain - 1), 'monotonic');
  }
  assert.equal(micGainDb(-5), 0);
  assert.equal(micGainDb(250), 24);
});

test('status: a complete answer parses, optional members get honest defaults', () => {
  const status = parseAudioStatus(wire());
  assert.equal(status.output.activeRoute, 'speaker');
  assert.equal(status.input.gainDb, 24);
  assert.equal(status.revision, 7);

  const bare = wire();
  delete bare.inputAvailable; delete bare.settingsSaved; delete bare.revision; delete bare.input.gainDb;
  bare.input.gain = 50;
  const parsed = parseAudioStatus(bare);
  assert.deepEqual([parsed.inputAvailable, parsed.settingsSaved, parsed.revision, parsed.input.gainDb], [true, true, 0, 12]);
});

test('status: anything malformed is refused whole', () => {
  const broken = [
    null, [], 'x', {}, { ...wire(), output: null },
    { ...wire(), output: { ...wire().output, route: 'bluetooth' } },
    { ...wire(), output: { ...wire().output, activeRoute: undefined } },
    { ...wire(), output: { ...wire().output, volume: 40.5 } },
    { ...wire(), output: { ...wire().output, volume: 101 } },
    { ...wire(), output: { ...wire().output, muted: 'no' } },
    { ...wire(), input: { ...wire().input, route: 'line' } },
    { ...wire(), input: { ...wire().input, gain: -1 } },
    { ...wire(), available: 1 },
  ];
  for (const raw of broken) assert.equal(parseAudioStatus(raw), null);
});

test('labels: every route the device can name has Chinese copy', () => {
  assert.deepEqual(OUTPUT_ROUTES.map(r => r.id), ['auto', 'headphones', 'speaker', 'both', 'off']);
  assert.deepEqual(INPUT_ROUTES.map(r => r.id), ['main', 'headset', 'both', 'off']);
  assert.equal(outputRouteLabel('speaker'), '扬声器');
  assert.equal(inputRouteLabel('headset'), '耳机麦克风');
  assert.equal(activeOutputLabel({ route: 'auto', activeRoute: 'headphones' }), '自动 · 耳机');
  assert.equal(activeOutputLabel({ route: 'both', activeRoute: 'both' }), '耳机+扬声器');
  assert.equal(activeOutputLabel({ route: 'off', activeRoute: 'off' }), '输出已关闭');
});

test('card summary: level, resolved route, jack', () => {
  const status = parseAudioStatus(wire());
  assert.equal(audioSummary(status), '40% · 自动 · 扬声器');
  status.output.muted = true; status.output.headphones = true; status.output.activeRoute = 'headphones';
  assert.equal(audioSummary(status), '静音 · 自动 · 耳机 · 耳机已插入');
  status.available = false;
  assert.equal(audioSummary(status), '音频编解码器未就绪');
});

test('eye scene: real route, silent when muted or closed, accepted by the device parser', () => {
  assert.equal(sceneAudioRoute({ activeRoute: 'headphones', muted: false }), 'headphones');
  assert.equal(sceneAudioRoute({ activeRoute: 'both', muted: false }), 'both');
  assert.equal(sceneAudioRoute({ activeRoute: 'speaker', muted: true }), 'mute');
  assert.equal(sceneAudioRoute({ activeRoute: 'off', muted: false }), 'mute');
  assert.equal(sceneAudioRoute({ activeRoute: 'auto', muted: false }), 'speaker');

  const output = parseAudioStatus(wire()).output;
  output.activeRoute = 'headphones'; output.volume = 64;
  const payload = audioScene({ volume: output.volume, muted: output.muted, device: '', route: sceneAudioRoute(output), title: activeOutputLabel(output) });
  assert.deepEqual(payload, { percent: 64, title: '自动 · 耳机', audio_route: 'headphones' });
  assert.deepEqual(sanitizeScenePayload(payload), payload);
  assert.deepEqual(audioScene({ volume: 64, muted: false, device: '', route: 'mute', title: '输出已关闭' }), { percent: 0, title: '输出已关闭', audio_route: 'mute' });
  // Without the codec service the older payload is unchanged.
  assert.deepEqual(audioScene({ volume: 64, muted: false, device: '/dev/audio/pcm0' }), { percent: 64, title: '/dev/audio/pcm0', audio_route: 'speaker' });
});

test('an unknown topic is a firmware without the service, anything else is an error', () => {
  assert.equal(isAudioUnsupported({ code: 'ENOTFOUND' }), true);
  assert.equal(isAudioUnsupported({ code: 'EUNAVAILABLE' }), false);
  assert.equal(isAudioUnsupported(new Error('ENOTFOUND')), false);
  assert.equal(isAudioUnsupported(null), false);
});
