import test from 'node:test';
import assert from 'node:assert/strict';
import { NyaLinkClient } from '../dist/index.js';

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));

/* Answers sys.hello at once and every other request after `answerAfterMs`. */
function installSocket(answerAfterMs) {
  class Socket {
    static OPEN = 1;
    readyState = 1;
    constructor() { queueMicrotask(() => this.onopen?.()); }
    send(raw) {
      const request = JSON.parse(raw);
      const reply = () => this.onmessage?.({ data: JSON.stringify({ v: 1, id: request.id, topic: request.topic, type: 'res',
        data: request.topic === 'sys.hello' ? { role: 'owner', capabilities: [] } : { ok: true } }) });
      if (request.topic === 'sys.hello') queueMicrotask(reply);
      else setTimeout(reply, answerAfterMs);
    }
    close() { this.readyState = 3; queueMicrotask(() => this.onclose?.()); }
  }
  globalThis.WebSocket = Socket;
}

async function connected(options) {
  const client = new NyaLinkClient(options);
  client.connect('ws://fixture');
  const deadline = Date.now() + 1000;
  while (client.state !== 'connected' && Date.now() < deadline) await delay(2);
  assert.equal(client.state, 'connected');
  return client;
}

test('per-request timeout outlives the client default', async () => {
  installSocket(60);
  const client = await connected({ requestTimeoutMs: 20 });
  try {
    await assert.rejects(client.request('slow.topic'), { code: 'ETIMEDOUT' });
    assert.deepEqual(await client.request('slow.topic', {}, { timeoutMs: 500 }), { ok: true });
  } finally { client.close(); }
});

test('per-request timeout can be shorter than the default', async () => {
  installSocket(60);
  const client = await connected({ requestTimeoutMs: 500 });
  try {
    await assert.rejects(client.request('slow.topic', {}, { timeoutMs: 10 }), { code: 'ETIMEDOUT' });
    assert.deepEqual(await client.request('slow.topic'), { ok: true });
  } finally { client.close(); }
});
