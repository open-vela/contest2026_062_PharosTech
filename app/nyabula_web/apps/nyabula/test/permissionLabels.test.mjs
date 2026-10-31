import test from 'node:test';
import assert from 'node:assert/strict';
import { permissionZh, permissionLabel, roleLabel } from '../../../packages/ui/src/permissions.ts';

test('all native runtime permissions have Chinese display names', () => {
  for (const id of ['core.log', 'storage.read', 'storage.write', 'network.request', 'ui.notify', 'ui.page', 'ui.html', 'ai.invoke', 'bluetooth.read', 'bluetooth.connect', 'bluetooth.call', 'audio.read', 'audio.control', 'audio.play', 'audio.capture', 'usb.access', 'camera.capture']) {
    assert.match(permissionZh(id), /[\u4e00-\u9fff]/);
    assert.notEqual(permissionZh(id), id);
  }
});

test('wire IDs remain separate from localized labels', () => {
  assert.equal(permissionLabel('ui.page', 'zh-CN'), '组件页面');
  assert.equal(permissionLabel('ui.page', 'en'), 'Component page');
  assert.equal(permissionZh('thirdparty.unknown'), '自定义权限');
  assert.equal(roleLabel('owner'), '主人');
  assert.equal(roleLabel('family'), '家人');
  assert.equal(roleLabel('guest'), '访客');
  assert.equal(roleLabel('guest', 'en'), 'Guest');
});
