import test from 'node:test';
import assert from 'node:assert/strict';
import { NyaLinkClient } from '../dist/index.js';

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(check) {
  const deadline = Date.now() + 1000;
  while (!check() && Date.now() < deadline) await delay(2);
  assert.ok(check());
}

test('authentication rejection stops retries until explicit reconnect', async () => {
  let sockets = 0;
  class Socket {
    static OPEN = 1;
    readyState = 1;
    constructor() { sockets++; queueMicrotask(() => this.onopen?.()); }
    send(raw) {
      const request = JSON.parse(raw);
      const allowed = request.data.token === 'valid-fixture-token';
      const data = allowed ? { role: 'owner', device: { id: 'fixture', name: 'Fixture' }, capabilities: [] } : { code: 'EACCES' };
      queueMicrotask(() => this.onmessage?.({ data: JSON.stringify({ v: 1, id: request.id,
        topic: request.topic, type: allowed ? 'res' : 'err', data }) }));
    }
    close() { this.readyState = 3; queueMicrotask(() => this.onclose?.()); }
  }
  globalThis.WebSocket = Socket;
  let token = null;
  const client = new NyaLinkClient({ loadToken: () => token, reconnectBaseMs: 5 });
  try {
    client.connect('ws://fixture');
    await until(() => client.state === 'closed');
    assert.equal(client.authError?.code, 'EACCES');
    await delay(30);
    assert.equal(sockets, 1);
    token = 'valid-fixture-token';
    client.connect('ws://fixture');
    await until(() => client.state === 'connected');
    assert.equal(client.authError, null);
    assert.equal(sockets, 2);
  } finally { client.close(); }
});

test('transient hello failure still reconnects', async () => {
  let sockets = 0;
  class Socket {
    static OPEN = 1;
    readyState = 1;
    constructor() { sockets++; queueMicrotask(() => this.onopen?.()); }
    send(raw) {
      const request = JSON.parse(raw);
      queueMicrotask(() => this.onmessage?.({ data: JSON.stringify({ v: 1, id: request.id,
        topic: request.topic, type: sockets === 1 ? 'err' : 'res',
        data: sockets === 1 ? { code: 'ENETDOWN' } : { role: 'owner', capabilities: [] } }) }));
    }
    close() { this.readyState = 3; queueMicrotask(() => this.onclose?.()); }
  }
  globalThis.WebSocket = Socket;
  const client = new NyaLinkClient({ reconnectBaseMs: 5 });
  try {
    client.connect('ws://fixture');
    await until(() => client.state === 'connected');
    assert.equal(sockets, 2);
    assert.equal(client.authError, null);
  } finally { client.close(); }
});
