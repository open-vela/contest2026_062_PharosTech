import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'vite';
import vue from '@vitejs/plugin-vue';
import { createSSRApp, h } from 'vue';
import { renderToString } from 'vue/server-renderer';

test('agent replies render Markdown even when their run failed; owner text stays literal', async () => {
  const server = await createServer({ configFile: false, plugins: [vue()],
    server: { middlewareMode: true }, ssr: { noExternal: [/^@nyabula\//] } });
  try {
    const { default: ChatThread } = await server.ssrLoadModule('/src/views/agent/ChatThread.vue');
    const text = '**读取确认**\n\n- 版本 `1.0.16`\n- 刷新成功';
    const messages = [
      { id: 'owner', role: 'user', text, pending: false, at: 0 },
      { id: 'success', role: 'agent', text, pending: false, at: 1 },
      { id: 'failed', role: 'agent', text: text + '\n\n<img src=x onerror=alert(1)> [危险](javascript:alert)', error: text, pending: false, at: 2 },
    ];
    const html = await renderToString(createSSRApp({ render: () => h(ChatThread, { messages, connected: true }) }));
    assert.equal((html.match(/<strong[^>]*>读取确认<\/strong>/g) || []).length, 2);
    assert.equal((html.match(/<code[^>]*>1\.0\.16<\/code>/g) || []).length, 2);
    assert.equal((html.match(/<ul[^>]*>/g) || []).length, 2);
    assert.match(html, /\*\*读取确认\*\*/);
    assert.doesNotMatch(html, /<img\b|href="javascript:/);
    assert.match(html, /&lt;img/);
    assert.ok([...html.matchAll(/class="([^"]*)"/g)].some(([, names]) =>
      ['msg', 'agent', 'error'].every(name => names.split(' ').includes(name))));
  } finally {
    await server.close();
  }
});
