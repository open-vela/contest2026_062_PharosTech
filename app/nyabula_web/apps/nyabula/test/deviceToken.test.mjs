import test from 'node:test';
import assert from 'node:assert/strict';
import { isDeviceToken, takeTokenFromQuery } from '../src/lib/deviceToken.ts';

// Fixture only: not a real device secret.
const TOKEN = '0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef';

test('a device token is exactly 64 hex characters', () => {
  assert.equal(isDeviceToken(TOKEN), true);
  assert.equal(isDeviceToken(TOKEN.slice(1)), false);
  assert.equal(isDeviceToken(TOKEN + '0'), false);
  assert.equal(isDeviceToken('g' + TOKEN.slice(1)), false);
  assert.equal(isDeviceToken(` ${TOKEN}`), false);
  assert.equal(isDeviceToken(`${TOKEN}\n`), false);
  for (const bad of ['', null, undefined, 64, [TOKEN]]) assert.equal(isDeviceToken(bad), false);
});

test('the token is split off the route query', () => {
  assert.deepEqual(takeTokenFromQuery({ token: TOKEN, stay: '1' }), { token: TOKEN, present: true, rest: { stay: '1' } });
  // Repeated parameter: vue-router hands over an array, the first one counts.
  assert.deepEqual(takeTokenFromQuery({ token: [TOKEN, 'x'] }), { token: TOKEN, present: true, rest: {} });
});

test('a malformed token is still stripped but never adopted', () => {
  assert.deepEqual(takeTokenFromQuery({ token: 'nope', a: 'b' }), { token: null, present: true, rest: { a: 'b' } });
  assert.deepEqual(takeTokenFromQuery({ token: null }), { token: null, present: true, rest: {} });
  assert.deepEqual(takeTokenFromQuery({ a: 'b' }), { token: null, present: false, rest: { a: 'b' } });
});
