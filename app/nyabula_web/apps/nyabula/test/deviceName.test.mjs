import test from 'node:test';
import assert from 'node:assert/strict';
import { parseDeviceName, utf8Length, validateDeviceName } from '../src/lib/deviceName.ts';

test('name length is counted in UTF-8 bytes', () => {
  assert.equal(utf8Length('Nyabula'), 7);
  assert.equal(utf8Length('猫'), 3);
  assert.equal(validateDeviceName('a'.repeat(32)), null);
  assert.notEqual(validateDeviceName('a'.repeat(33)), null);
  assert.equal(validateDeviceName('猫'.repeat(10)), null); // 30 bytes
  assert.notEqual(validateDeviceName('猫'.repeat(11)), null); // 33 bytes
});

test('empty resets to the default name; control characters are refused', () => {
  assert.equal(validateDeviceName(''), null);
  assert.notEqual(validateDeviceName('a\nb'), null);
  assert.notEqual(validateDeviceName('a' + String.fromCharCode(0x7f) + 'b'), null);
});

test('network.status name fields', () => {
  assert.deepEqual(parseDeviceName({ name: 'Nyabula-1A2B', named: false }), { name: 'Nyabula-1A2B', named: false });
  assert.deepEqual(parseDeviceName({ name: '客厅的猫', named: true }), { name: '客厅的猫', named: true });
  for (const junk of [null, undefined, 7, {}, { name: '' }, { name: 3, named: 'yes' }]) {
    assert.deepEqual(parseDeviceName(junk), { name: null, named: false });
  }
});
