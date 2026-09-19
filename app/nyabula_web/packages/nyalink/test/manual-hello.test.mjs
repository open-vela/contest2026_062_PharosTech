import test from 'node:test';
import assert from 'node:assert/strict';
import { NyaLinkClient, NyaLinkError } from '../dist/index.js';

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(check) {
  const deadline = Date.now() + 1000;
  while (!check() && Date.now() < deadline) await delay(2);
  assert.ok(check());
}

// Fixtures only: not real device secrets.
const SESSION = 'fixture-session-token';
const PASSWORD = 'fixture-password';

/** Fake device: sys.auth.state / sys.login before hello, hello by token. */
function fakeDevice() {
  const log = { sockets: [], topics: [] };
  class Socket {
    static OPEN = 1;
    readyState = 1;
    constructor() { log.sockets.push(this); queueMicrotask(() => this.onopen?.()); }
    reply(request, type, data) {
      queueMicrotask(() => this.onmessage?.({ data: JSON.stringify({ v: 1, id: request.id, topic: request.topic, type, data }) }));
    }
    send(raw) {
      const request = JSON.parse(raw);
      log.topics.push(request.topic);
      if (request.topic === 'sys.auth.state') return this.reply(request, 'res', { passwordSet: true, locked: false, retryAfterMs: 0 });
      if (request.topic === 'sys.login') {
        if (request.data.password === PASSWORD) return this.reply(request, 'res', { token: SESSION });
        return this.reply(request, 'err', { code: 'ELOCKED', message: 'locked', retryAfterMs: 4200 });
      }
      if (request.topic === 'sys.hello') {
        if (request.data.token === SESSION) return this.reply(request, 'res', { role: 'owner', capabilities: [], auth: 'session', passwordSet: true });
        this.reply(request, 'err', { code: 'EACCES' });
        return;
      }
      this.reply(request, 'res', {});
    }
    close() { this.readyState = 3; queueMicrotask(() => this.onclose?.()); }
  }
  globalThis.WebSocket = Socket;
  return log;
}

test('manualHello waits on open, serves pre-auth requests, then authenticates', async () => {
  const log = fakeDevice();
  const client = new NyaLinkClient({ manualHello: true, reconnectBaseMs: 5 });
  try {
    client.connect('ws://fixture');
    await until(() => client.awaitingAuth);
    assert.equal(client.state, 'authenticating');
    assert.deepEqual(log.topics, []); // no automatic hello
    const state = await client.request('sys.auth.state');
    assert.equal(state.passwordSet, true);
    const { token } = await client.request('sys.login', { password: PASSWORD });
    const hello = await client.authenticate(token);
    assert.equal(hello.auth, 'session');
    assert.equal(client.state, 'connected');
    assert.equal(client.awaitingAuth, false);
    assert.equal(client.auth, 'session');
    assert.equal(client.passwordSet, true);
    assert.deepEqual(log.topics, ['sys.auth.state', 'sys.login', 'sys.hello']);
    assert.equal(log.sockets.length, 1);
  } finally { client.close(); }
});

test('err frames keep their extra data and a failed login keeps the socket', async () => {
  fakeDevice();
  const client = new NyaLinkClient({ manualHello: true });
  try {
    client.connect('ws://fixture');
    await until(() => client.awaitingAuth);
    await assert.rejects(client.request('sys.login', { password: 'wrong-fixture' }), (e) => {
      assert.ok(e instanceof NyaLinkError);
      assert.equal(e.code, 'ELOCKED');
      assert.equal(e.data.retryAfterMs, 4200);
      return true;
    });
    assert.equal(client.awaitingAuth, true);
    assert.equal(client.state, 'authenticating');
  } finally { client.close(); }
});

test('an idle pre-auth socket closed by the device is not reopened', async () => {
  const log = fakeDevice();
  const client = new NyaLinkClient({ manualHello: true, reconnectBaseMs: 5 });
  try {
    client.connect('ws://fixture');
    await until(() => client.awaitingAuth);
    log.sockets[0].close(); // device-side idle timeout
    await until(() => client.state === 'closed');
    assert.equal(client.authError, null);
    assert.equal(client.awaitingAuth, false);
    await delay(40);
    assert.equal(log.sockets.length, 1);
    // The host reopens one when it needs it.
    client.connect('ws://fixture');
    await until(() => client.awaitingAuth);
    assert.equal(log.sockets.length, 2);
  } finally { client.close(); }
});

test('a supplied token is sent on open and reused on reconnect', async () => {
  const log = fakeDevice();
  const client = new NyaLinkClient({ manualHello: true, reconnectBaseMs: 5, loadToken: () => 'ignored-fixture' });
  try {
    client.setToken(SESSION);
    client.connect('ws://fixture');
    await until(() => client.state === 'connected');
    assert.equal(client.awaitingAuth, false);
    log.sockets[0].close(); // link drop after authentication
    await until(() => log.sockets.length === 2 && client.state === 'connected');
    assert.deepEqual(log.topics, ['sys.hello', 'sys.hello']);
  } finally { client.close(); }
});

test('authenticate rejects with the device error and stops retries on EACCES', async () => {
  const log = fakeDevice();
  const client = new NyaLinkClient({ manualHello: true, reconnectBaseMs: 5 });
  try {
    client.connect('ws://fixture');
    await until(() => client.awaitingAuth);
    await assert.rejects(client.authenticate('stale-fixture-token'), (e) => e instanceof NyaLinkError && e.code === 'EACCES');
    assert.equal(client.state, 'closed');
    assert.equal(client.authError?.code, 'EACCES');
    await delay(30);
    assert.equal(log.sockets.length, 1);
    await assert.rejects(client.authenticate(SESSION), (e) => e.code === 'ENOTCONN');
  } finally { client.close(); }
});
