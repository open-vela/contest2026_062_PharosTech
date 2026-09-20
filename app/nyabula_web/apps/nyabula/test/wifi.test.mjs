import test from 'node:test';
import assert from 'node:assert/strict';
import { bandLabel, isLinkDropError, joinFailed, normalizeScan, parseNetworkStatus, validateWifiInput } from '../src/lib/wifi.ts';

const HOME = { ssid: 'Home', rssi: -48, freq: 5180, secure: true };
const coded = (code) => Object.assign(new Error('x'), { code });

test('scan results: wrapper key or bare array', () => {
  assert.deepEqual(normalizeScan([HOME]), [HOME]);
  assert.deepEqual(normalizeScan({ networks: [HOME] }), [HOME]);
  assert.deepEqual(normalizeScan({ count: 1, results: [HOME] }), [HOME]);
  for (const junk of [null, undefined, 'x', 7, {}, { networks: 'x' }]) assert.deepEqual(normalizeScan(junk), []);
});

test('scan results: strongest per SSID, sorted by signal, hidden dropped', () => {
  const list = normalizeScan({ networks: [
    { ssid: 'Cafe', rssi: -80, freq: 2412, secure: false },
    { ssid: 'Home', rssi: -70, freq: 2437, secure: true },
    HOME,
    { ssid: '', rssi: -30, freq: 2412, secure: true },
    { ssid: '\0\0\0', rssi: -31, freq: 2412, secure: true },
    { ssid: '   ', rssi: -32 },
    { rssi: -33 },
    'garbage',
    { ssid: 'NoSignal' },
  ] });
  assert.deepEqual(list.map((n) => n.ssid), ['Home', 'Cafe', 'NoSignal']);
  assert.deepEqual(list[0], HOME);
  assert.deepEqual(list[2], { ssid: 'NoSignal', rssi: null, freq: null, secure: null });
});

test('scan results: security flag variants', () => {
  const list = normalizeScan([
    { ssid: 'a', rssi: -1, secure: 1 },
    { ssid: 'b', rssi: -2, secure: 0 },
    { ssid: 'c', rssi: -3, security: 'WPA2' },
    { ssid: 'd', rssi: -4, auth: 'open' },
  ]);
  assert.deepEqual(list.map((n) => n.secure), [true, false, true, false]);
});

test('credentials: 8-63 chars when protected, empty allowed when open or unknown', () => {
  assert.equal(validateWifiInput('Home', 'password', true), null);
  assert.equal(validateWifiInput('Home', 'x'.repeat(63), true), null);
  assert.notEqual(validateWifiInput('Home', '', true), null);
  assert.notEqual(validateWifiInput('Home', 'short', true), null);
  assert.notEqual(validateWifiInput('Home', 'x'.repeat(64), true), null);
  assert.equal(validateWifiInput('Cafe', '', false), null);
  assert.equal(validateWifiInput('Typed', '', null), null);
  assert.notEqual(validateWifiInput('Typed', 'short', null), null);
  assert.notEqual(validateWifiInput('', 'password', true), null);
  assert.notEqual(validateWifiInput('猫'.repeat(11), 'password', true), null); // 33 bytes
  assert.equal(validateWifiInput('猫'.repeat(10), 'password', true), null);
});

test('network.status parsing is defensive', () => {
  assert.deepEqual(parseNetworkStatus(null), { state: null, ifname: null, configured: false, error: null, attempt: 0, ssid: null, ipv4: null, rssi: null, hostname: null, ap: null });
  const s = parseNetworkStatus({ state: 'ap_provision', ifname: 'wlan0', configured: true, error: '', attempt: 2, ssid: 'Home', ap: { ssid: 'Nyabula-1', psk: 'x', ipv4: '192.168.4.1' } });
  assert.deepEqual(s, { state: 'ap_provision', ifname: 'wlan0', configured: true, error: null, attempt: 2, ssid: 'Home', ipv4: null, rssi: null, hostname: null, ap: { ssid: 'Nyabula-1', ipv4: '192.168.4.1' } });
  const online = parseNetworkStatus({ state: 'sta_online', ssid: 'Home', ipv4: '192.168.1.7', rssi: -61, hostname: 'nyabula-cat' });
  assert.deepEqual([online.rssi, online.hostname], [-61, 'nyabula-cat']);
  assert.equal(parseNetworkStatus({ rssi: '-61' }).rssi, null);
  assert.equal(parseNetworkStatus({ state: 'bogus' }).state, null);
});

test('join failure = credentials stored but the device is back on its hotspot', () => {
  assert.equal(joinFailed(parseNetworkStatus({ state: 'ap_provision', configured: true })), true);
  assert.equal(joinFailed(parseNetworkStatus({ state: 'sta_failed', configured: true })), true);
  assert.equal(joinFailed(parseNetworkStatus({ state: 'ap_provision', configured: false })), false);
  assert.equal(joinFailed(parseNetworkStatus({ state: 'sta_connecting', configured: true })), false);
  assert.equal(joinFailed(parseNetworkStatus({ state: 'sta_online', configured: true })), false);
});

test('only a drop after sending counts as the expected hotspot teardown', () => {
  assert.equal(isLinkDropError(coded('ECONNRESET')), true);
  assert.equal(isLinkDropError(coded('ETIMEDOUT')), true);
  for (const code of ['ENOTCONN', 'EOFFLINE', 'EPERM', 'EINVAL']) assert.equal(isLinkDropError(coded(code)), false);
  assert.equal(isLinkDropError(null), false);
});

test('band label', () => {
  assert.deepEqual([bandLabel(2437), bandLabel(5180), bandLabel(5955), bandLabel(null)], ['2.4 GHz', '5 GHz', '6 GHz', '']);
});

test('a background scan is told apart by its sequence number', async () => {
  const { isScanning, isUnknownTopic, scanSeq } = await import('../src/lib/wifi.ts');
  assert.equal(scanSeq({ seq: 3, scanning: false, networks: [] }), 3);
  assert.equal(scanSeq({ networks: [] }), 0);
  assert.equal(scanSeq(null), 0);
  assert.equal(isScanning({ scanning: true }), true);
  assert.equal(isScanning({ scanning: 'yes' }), false);
  assert.equal(isUnknownTopic({ code: 'ENOTFOUND' }), true);
  assert.equal(isUnknownTopic({ code: 'ECONNRESET' }), false);
});
