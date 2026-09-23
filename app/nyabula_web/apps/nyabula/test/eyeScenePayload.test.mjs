import test from 'node:test';
import assert from 'node:assert/strict';
import {
  alarmScene, audioScene, briefingScene, calendarCountdown, calendarScene, captionScene, clipText, companionScene,
  coreSceneName, devicesScene, hardwareCounts, memoryScene, musicScene, networkScene, sanitizeScenePayload,
  sleepTimerScene, stopwatchScene, systemScene, taskScene, timerScene, weatherScene, wrapText,
} from '../src/composables/eyeScenePayload.ts';
import { FEATURES } from '../src/views/services/features/index.ts';

const utf8 = (s) => new TextEncoder().encode(s).length;
/** A builder's output must survive the store's sanitizer unchanged: no unknown keys, no bad numbers. */
const accepted = (payload) => assert.deepEqual(sanitizeScenePayload(payload), Object.fromEntries(Object.entries(payload).filter(([, v]) => v !== undefined)));

test('scene names: kebab feature types map to wire names, non-scenes to null', () => {
  assert.equal(coreSceneName('sleep-timer'), 'sleep_timer');
  assert.equal(coreSceneName('music'), 'music');
  for (const type of ['pairing', 'sleep', 'none', '']) assert.equal(coreSceneName(type), null);
});

test('sanitize: unknown keys dropped, integers rounded and clamped, bad enums dropped', () => {
  assert.deepEqual(sanitizeScenePayload({
    ssid: 'Home', rssi: -40, level: 3, title: 'ok', remaining_ms: 1500.6, percent: 300, hour: -2, progress: 0.25,
    task_state: 'cancelled', audio_route: 'mute', active: 'yes', playing: true, year: NaN, detail: null, eq_bands: [1, 'x'],
  }), {
    title: 'ok', remaining_ms: 1501, percent: 255, hour: 0, progress: 0.25, audio_route: 'mute', playing: true,
    eq_bands: [1, 0, 0, 0, 0, 0, 0, 0, 0, 0],
  });
  assert.deepEqual(sanitizeScenePayload(null), {});
});

test('text: clipped on code point boundaries inside the device buffers', () => {
  const long = '主人喜欢在早上喝黑咖啡，不加糖，周末还会多睡一会儿。';
  const clipped = clipText(long);
  assert.ok(utf8(clipped) <= 47 && clipped.endsWith('…') && !clipped.includes('�'));
  assert.equal(clipText('  短  句 '), '短 句');
  assert.ok(utf8(sanitizeScenePayload({ value: '1234567890123456789012345' }).value) <= 23);
  const lines = wrapText(long, 2);
  assert.equal(lines.length, 2);
  assert.ok(lines.every((l) => utf8(l) <= 47));
  assert.ok(long.startsWith(lines[0]));
  assert.deepEqual(wrapText('', 2), []);
});

test('timer: whole seconds, never the 0 that starts the device demo; finished draws 00:00', () => {
  const running = timerScene({ durationMs: 300000, remainingMs: 61234.5, running: true, finished: false, label: '泡茶' });
  assert.deepEqual(running, { duration_ms: 300000, remaining_ms: 62000, active: true, title: '泡茶', detail: '泡茶' });
  accepted(running);
  assert.equal(timerScene({ durationMs: 60000, remainingMs: 0.2, running: true, finished: false, label: '' }).remaining_ms, 1000);
  assert.equal(timerScene({ durationMs: 60000, remainingMs: 0, running: false, finished: false, label: '' }).remaining_ms, 1);
  assert.equal(timerScene({ durationMs: 60000, remainingMs: 30000, running: false, finished: false, label: '' }).detail, '已暂停');
  const done = timerScene({ durationMs: 60000, remainingMs: 0, running: false, finished: true, label: '' });
  assert.deepEqual(done, { duration_ms: 1, remaining_ms: 0, active: false, title: '倒计时', detail: '时间到' });
  accepted(done);
});

test('stopwatch and sleep timer', () => {
  assert.deepEqual(stopwatchScene({ elapsedMs: 0, running: false }), { elapsed_ms: 1, active: false, title: '秒表' });
  assert.deepEqual(stopwatchScene({ elapsedMs: 12345.9, running: true, label: '跑步' }), { elapsed_ms: 12300, active: true, title: '跑步' });
  // The device shows ceil(minutes): the payload only changes once a minute.
  assert.equal(sleepTimerScene({ remainingMs: 29 * 60000 + 1, running: true }).remaining_ms, 30 * 60000);
  assert.equal(sleepTimerScene({ remainingMs: 29 * 60000 + 59999, running: true }).remaining_ms, 30 * 60000);
  assert.equal(sleepTimerScene({ remainingMs: 10, running: true }).remaining_ms, 60000);
  accepted(sleepTimerScene({ remainingMs: 1800000, running: true }));
});

test('alarm: hour/minute/title; real time repeated where the device would draw its default', () => {
  assert.deepEqual(alarmScene({ time: '06:45', label: '起床' }), { hour: 6, minute: 45, title: '起床', detail: '起床', alarm_copy: 'name', active: false });
  assert.equal(alarmScene({ time: '07:00', label: '起床', ringing: true }).title, '07:00 起床');
  assert.equal(alarmScene({ time: '07:00', label: '', ringing: true }).active, true);
  assert.equal(alarmScene({ time: '00:10', label: '' }).title, '00:10 下一个闹钟');
  for (const bad of ['', '7', '24:00', '12:60', 'ab:cd']) assert.equal(alarmScene({ time: bad, label: '' }), null);
  accepted(alarmScene({ time: '23:59', label: 'x' }));
});

test('calendar: local date fields and countdown', () => {
  const now = new Date(2026, 8, 19, 15, 0).getTime();
  const allDay = calendarScene({ title: '小猫生日', startAt: new Date(2026, 8, 22).getTime(), now });
  assert.deepEqual(allDay, { year: 2026, month: 9, day: 22, hour: 0, minute: 0, title: '小猫生日', detail: '全天 · 还有 3 天' });
  accepted(allDay);
  const timed = calendarScene({ title: '评审', startAt: new Date(2026, 8, 19, 16, 30).getTime(), now });
  assert.deepEqual([timed.hour, timed.minute, timed.detail], [16, 30, '就是今天']);
  assert.equal(calendarCountdown(new Date(2026, 8, 20, 0, 0).getTime(), now), '明天');
  assert.equal(calendarCountdown(new Date(2026, 8, 17, 23, 0).getTime(), now), '已过 2 天');
});

test('task: wire progress is 0..1 and never exactly 0; unknown states fall back', () => {
  assert.deepEqual(taskScene({ title: '整理照片', progress: 40, state: 'running' }), { title: '整理照片', progress: 0.4, task_state: 'running', value: '40%' });
  assert.equal(taskScene({ title: 'x', progress: 0, state: 'queued' }).progress, 0.001);
  assert.equal(taskScene({ title: 'x', progress: 250, state: 'done' }).progress, 1);
  assert.equal(taskScene({ title: 'x', progress: 10, state: 'cancelled' }).task_state, 'failed');
  assert.equal(taskScene({ title: 'x', progress: 10, state: '??' }).task_state, 'queued');
  accepted(taskScene({ title: 'x', progress: 55.5, state: 'confirm' }));
});

test('memory: short text keeps the tag as subtitle, long text wraps over two lines', () => {
  assert.deepEqual(memoryScene({ text: '喜欢黑咖啡', tag: '日常' }), { title: '喜欢黑咖啡', subtitle: '日常', detail: '设备记忆' });
  const long = memoryScene({ text: '周末全家一起看了电影，大家都很开心，还约好下周再去一次。', tag: '家人' });
  assert.ok(long.subtitle && long.subtitle !== '家人');
  assert.equal(long.detail, '家人');
  accepted(long);
});

test('briefing / companion / devices', () => {
  assert.deepEqual(briefingScene({ index: 1, count: 4, title: '今日天气', source: '和风天气', playing: true }),
    { briefing_index: 2, briefing_count: 4, title: '今日天气', subtitle: '和风天气', playing: true });
  assert.equal(briefingScene({ index: 9, count: 3, title: 't', source: 's', playing: false }).briefing_index, 3);
  assert.equal(briefingScene({ index: 0, count: 3, title: 't', source: 's', playing: false }).detail, '第 1 / 3 条');

  assert.equal(companionScene({ enabled: false, mode: 'quiet', quietNow: false, dailyCount: 0, dailyLimit: 3 }).subtitle, '主动陪伴已关闭');
  assert.equal(companionScene({ enabled: true, mode: 'story', quietNow: true, dailyCount: 0, dailyLimit: 3 }).subtitle, '免打扰中');
  assert.deepEqual(companionScene({ enabled: true, mode: 'interactive', quietNow: false, dailyCount: 1, dailyLimit: 3 }),
    { title: '适度互动', active: true, subtitle: '今日 1/3 次', detail: '结合日程与天气' });

  const snapshot = {
    audioDevices: [{ available: true }, { available: false }], storage: [{ available: true }],
    network: { interfaces: [{ up: true, loopback: true }, { up: true }, { up: false }] },
  };
  assert.deepEqual(hardwareCounts(snapshot), { audio: 1, storage: 1, network: 1, total: 3 });
  assert.deepEqual(devicesScene(snapshot), { device_count: 3, title: '3', subtitle: '个硬件在线', detail: '音频 1 · 存储 1 · 网络 1' });
  // 0 would show the device's demo "2": the count always travels as the title too.
  assert.equal(devicesScene(null).title, '0');
});

test('music / audio / network / system / caption / weather', () => {
  assert.deepEqual(musicScene({ track: 'a.wav', playing: true, elapsedMs: 61999, durationMs: 180000 }),
    { title: 'a.wav', playing: true, duration_ms: 180000, position_ms: 61000, music_view: 'spectrum' });
  assert.equal(musicScene({ track: '', playing: false, elapsedMs: 0, durationMs: 0 }).position_ms, 1);
  const lyric = musicScene({ track: 'a', playing: true, elapsedMs: 5000, durationMs: 9000, lyrics: { prev: '', current: '喵', next: '' } });
  assert.deepEqual([lyric.music_view, lyric.current_line], ['lyrics', '喵']);
  accepted(lyric);

  assert.deepEqual(audioScene({ volume: 64, muted: false, device: '/dev/audio/pcm0p' }), { percent: 64, title: '/dev/audio/pcm0p', audio_route: 'speaker' });
  assert.deepEqual(audioScene({ volume: 64, muted: true, device: '' }), { percent: 0, title: '播放输出', audio_route: 'mute' });

  assert.deepEqual(networkScene({ ssid: 'Home', rssi: -50, connected: true }), { title: 'Home', network_state: 'wifi', signal_good: true, detail: '-50 dBm' });
  assert.equal(networkScene({ ssid: 'Home', rssi: -80, connected: true }).signal_good, false);
  assert.deepEqual(networkScene({ ssid: null, rssi: null, connected: false }), { title: '未连接', network_state: 'offline', signal_good: false, detail: '' });

  const system = systemScene({ cpuPercent: 23.4, memoryPercent: 41.2, uptimeText: '3 小时 5 分', version: '0.2.0' });
  assert.deepEqual(system, { title: 'CPU 23%', subtitle: '内存 41% · 运行 3 小时 5 分', detail: 'Core 0.2.0', percent: 23 });
  assert.ok(/^[\x20-\x7e]+$/.test(system.title), 'system title is drawn with a Latin-only face');
  assert.equal(systemScene({ cpuPercent: null, memoryPercent: null, uptimeText: '', version: '' }).title, 'OPENVELA');

  assert.deepEqual(captionScene('你好', '上一句'), { previous_line: '上一句', current_line: '你好', next_line: '' });
  const wrapped = captionScene('今天天气很好，我们一起去公园散步，然后回家给小猫开一个罐头吧。');
  assert.ok(wrapped.next_line && utf8(wrapped.current_line) <= 47 && utf8(wrapped.next_line) <= 47);

  const weather = weatherScene({ kind: 'rain', city: '上海', temperature: 21.5, humidity: 80 });
  assert.deepEqual(sanitizeScenePayload(weather), { weather: 'rain', title: '上海', temperature_c: 21.5, feels_like_c: 21.5, humidity_percent: 80 });
});

test('feature registry: stages', () => {
  const planned = FEATURES.filter((f) => f.stage === 'planned').map((f) => f.type).sort();
  assert.deepEqual(planned, ['battery', 'call', 'eq', 'health', 'home', 'identity', 'presence', 'privacy', 'subwoofer']);
  assert.ok(FEATURES.every((f) => f.stage === 'ready' || f.stage === 'planned'));
  assert.ok(FEATURES.filter((f) => f.stage === 'planned').every((f) => f.needs));
  // Every ready feature is a device scene, except the two that are not scenes by design.
  const notScenes = FEATURES.filter((f) => f.stage === 'ready' && coreSceneName(f.type) === null).map((f) => f.type).sort();
  assert.deepEqual(notScenes, ['pairing', 'sleep']);
});
