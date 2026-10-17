<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue';
import { MdButton, MdTextField, useDialogStore } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import McpInboundPanel from './McpInboundPanel.vue';

type Kind = 'tools' | 'resources' | 'prompts';
interface Grant { kind: Kind; name: string }
interface Source { id: string; title: string; url: string; hasSecret: boolean; generation: number;
  grants: Grant[]; catalog?: Partial<Record<Kind, Record<string, unknown>[]>> }
const session = useSessionStore();
const dialog = useDialogStore();
const items = ref<Source[]>([]);
const revision = ref(0);
const busy = ref(false);
const loaded = ref(false);
const error = ref('');
const editing = ref(false);
const selected = ref<Source | null>(null);
const editRevision = ref(0);
const form = reactive({ id: '', title: '', url: '' });
const available = computed(() => session.connected && session.isOwner);
const kinds: { id: Kind; label: string }[] = [
  { id: 'tools', label: '工具' }, { id: 'resources', label: '资源' }, { id: 'prompts', label: '提示模板' },
];
const canSave = computed(() => available.value && !busy.value && /^[a-z0-9_-]{1,40}$/.test(form.id) &&
  !!form.title.trim() && new TextEncoder().encode(form.title).length <= 96 && /^https:\/\//.test(form.url));
function apply(data: Record<string, unknown>): void {
  if (!Array.isArray(data.items) || typeof data.revision !== 'number') throw new Error('设备返回了无效的 MCP 清单');
  items.value = data.items as Source[]; revision.value = data.revision; loaded.value = true;
}
async function request(topic: string, data: Record<string, unknown> = {}): Promise<void> {
  if (!available.value || busy.value) return;
  const client = session.client;
  busy.value = true;
  try {
    const result = await session.request(topic, data);
    if (client !== session.client) return;
    apply(result); error.value = '';
    if (topic === 'agent.mcp.out.save') editing.value = false;
  } catch (cause) {
    if (client === session.client) error.value = `${String(cause)}。操作结果未确认时，请刷新清单核对；不要重复提交。`;
  } finally { busy.value = false; }
}
function open(source: Source | null): void {
  selected.value = source; editRevision.value = revision.value;
  Object.assign(form, source ? { id: source.id, title: source.title, url: source.url } : { id: '', title: '', url: '' });
  editing.value = true;
}
function name(kind: Kind, row: Record<string, unknown>): string { return String(row[kind === 'resources' ? 'uri' : 'name'] ?? ''); }
function granted(source: Source, kind: Kind, id: string): boolean { return source.grants.some(g => g.kind === kind && g.name === id); }
async function toggle(source: Source, kind: Kind, id: string): Promise<void> {
  const exists = granted(source, kind, id);
  const expected = revision.value;
  if (!exists && !await dialog.confirm(`允许 Nyabot 读取 ${source.title} 中 ${id} 的定义，并提议调用。每次调用仍需单独批准；调用参数会发送给此外部服务。`,
    { title: '授权外部能力', confirmText: '授权' })) return;
  const grants = exists ? source.grants.filter(g => g.kind !== kind || g.name !== id) : [...source.grants, { kind, name: id }];
  await request('agent.mcp.out.grant', { id: source.id, revision: expected, generation: source.generation, grants });
}
async function refresh(source: Source): Promise<void> {
  const expected = revision.value;
  if (await dialog.confirm('Core 将连接此外部地址并读取目录。成功刷新会清除该服务的全部旧授权，需要重新核对后逐项授权。',
    { title: '刷新 MCP 目录', confirmText: '连接并刷新' }))
    await request('agent.mcp.out.refresh', { id: source.id, revision: expected });
}
async function remove(source: Source): Promise<void> {
  const expected = revision.value;
  if (await dialog.confirm('删除连接、凭据与授权。不会撤销外部服务已经收到或完成的操作。',
    { title: '删除 MCP 连接', danger: true, confirmText: '删除' }))
    await request('agent.mcp.out.delete', { id: source.id, revision: expected });
}
watch(() => session.client, () => {
  items.value = []; loaded.value = false; revision.value = 0; editing.value = false; selected.value = null;
  void request('agent.mcp.out.list');
}, { immediate: true });
watch(() => session.connected, connected => { if (connected) void request('agent.mcp.out.list'); });
</script>

<template>
  <section class="mcp-panel">
    <h2>MCP · 外部服务</h2>
    <p>Core 主动连接外部服务。仅支持 MCP 2025-11-25 Streamable HTTP；生产连接使用 HTTPS。最多 4 个服务，新连接默认无授权。</p>
    <p>外连配置不授予反向访问权限。密钥只允许通过设备本地 CLI 配置，不会返回网页；已有密钥的连接须在本地更换地址。</p>
    <p v-if="!available" role="status">连接设备并以主人身份管理 MCP。</p>
    <p v-if="error" role="alert" class="error">{{ error }}</p>
    <div class="actions"><MdButton :disabled="!available || !loaded || busy || items.length >= 4" @click="open(null)">添加 MCP 服务</MdButton>
      <MdButton variant="text" :disabled="!available || busy" @click="request('agent.mcp.out.list')">刷新连接清单</MdButton></div>
    <form v-if="editing" class="editor" @submit.prevent="canSave && request('agent.mcp.out.save', { ...form, revision: editRevision })">
      <h3>{{ selected ? '编辑连接' : '添加连接' }}</h3>
      <MdTextField v-model="form.id" label="服务 ID" :disabled="!!selected || busy" placeholder="小写字母、数字、短横线或下划线" />
      <MdTextField v-model="form.title" label="服务名称" :disabled="busy" />
      <MdTextField v-model="form.url" label="MCP HTTPS 地址" :disabled="busy || !!selected?.hasSecret" placeholder="https://example.com/mcp" />
      <p>保存后清除目录与授权。不要在地址中填写密码或访问令牌。</p>
      <div class="actions"><MdButton :disabled="!canSave">保存连接</MdButton><MdButton type="button" variant="text" :disabled="busy" @click="editing = false">关闭编辑器</MdButton></div>
    </form>
    <p v-if="loaded && !items.length">尚未添加外部服务。</p>
    <article v-for="source in items" :key="source.id" class="source">
      <h3>{{ source.title }}</h3><code>{{ source.id }} · {{ source.url }}</code>
      <p>{{ source.hasSecret ? '本地凭据已配置' : '无 Bearer 凭据' }} · 授权 {{ source.grants.length }} 项 · 目录版本 {{ source.generation }}</p>
      <div class="actions"><MdButton variant="tonal" :disabled="!available || busy" @click="refresh(source)">刷新目录并清除授权</MdButton>
        <MdButton variant="text" :disabled="!available || busy" @click="open(source)">编辑连接</MdButton>
        <MdButton variant="text" :disabled="!available || busy" @click="remove(source)">删除连接</MdButton></div>
      <p v-if="!source.catalog">尚未读取目录。请确认地址与凭据，再主动刷新。</p>
      <template v-for="kind in kinds" :key="kind.id">
        <section v-if="source.catalog?.[kind.id]?.length" class="catalog">
          <h4>{{ kind.label }}</h4>
          <article v-for="row in source.catalog[kind.id]" :key="name(kind.id, row)" class="entry">
            <strong>{{ name(kind.id, row) }}</strong>
            <p v-if="row.description">{{ row.description }}</p>
            <details><summary>查看完整定义 · 外部不可信内容</summary><pre>{{ JSON.stringify(row, null, 2) }}</pre></details>
            <MdButton variant="outlined" :disabled="!available || busy" @click="toggle(source, kind.id, name(kind.id, row))">{{ granted(source, kind.id, name(kind.id, row)) ? '撤销授权' : '授权此项' }}</MdButton>
          </article>
        </section>
      </template>
    </article>
    <p>撤销会阻止尚未派发的调用，不能取消远端已经收到的请求。远端失败可能已产生副作用，需人工核对，不自动重试。</p>
    <McpInboundPanel />
  </section>
</template>

<style scoped>
.mcp-panel { max-width: 960px; width: 100%; margin: 0 auto; line-height: 1.6; min-width: 0; }
h2 { font: 600 22px var(--font-title); }
.actions { display: flex; flex-wrap: wrap; gap: 8px; }
.editor { display: flex; flex-direction: column; gap: 12px; padding: 20px 0; }
.source { padding: 20px 0; border-bottom: 1px solid var(--md-outline-variant); }
.entry { padding: 12px; margin: 8px 0; border: 1px solid var(--md-outline-variant); border-radius: 12px; }
code, p, strong { overflow-wrap: anywhere; }
pre { white-space: pre-wrap; overflow-wrap: anywhere; max-height: 240px; overflow: auto; font-size: 13px; }
.error { color: var(--md-error); }
</style>
