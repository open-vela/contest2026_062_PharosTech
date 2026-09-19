import test from 'node:test';
import assert from 'node:assert/strict';
import { SELF_KEY, cloudKey, lanKey, parseDeviceKey, selfWsUrl } from '../src/lib/deviceKey.ts';

test('self key addresses the device that served the page', () => {
  assert.equal(SELF_KEY, 'self');
  assert.deepEqual(parseDeviceKey('self'), { transport: 'self', address: '' });
  // Only the bare key is valid: no address may ride along.
  assert.equal(parseDeviceKey('self:192.168.4.1'), null);
  assert.equal(parseDeviceKey('selfish'), null);
});

test('existing transports keep parsing', () => {
  assert.deepEqual(parseDeviceKey(lanKey('10.0.0.5')), { transport: 'lan', address: '10.0.0.5:7788' });
  assert.deepEqual(parseDeviceKey('lan:ws://host:1/nyalink'), { transport: 'lan', address: 'ws://host:1/nyalink' });
  assert.deepEqual(parseDeviceKey(cloudKey('dev-1')), { transport: 'cloud', address: 'dev-1' });
  assert.deepEqual(parseDeviceKey('dev:preview'), { transport: 'dev', address: 'preview' });
  for (const bad of ['', 'lan', 'lan:', 'ftp:host', ':host']) assert.equal(parseDeviceKey(bad), null);
});

test('self socket shares host and port with the page', () => {
  assert.equal(selfWsUrl({ protocol: 'http:', host: '192.168.4.1' }), 'ws://192.168.4.1/nyalink');
  assert.equal(selfWsUrl({ protocol: 'http:', host: '192.168.1.57:8080' }), 'ws://192.168.1.57:8080/nyalink');
  assert.equal(selfWsUrl({ protocol: 'https:', host: 'pet.example' }), 'wss://pet.example/nyalink');
});
