<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue';
import { MdButton, MdTextField, useDialogStore } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';

interface Client { id: string; title: string; expiresAt: number; scopes: string[] }
interface Audit { client: string; tool: string; at: number; status: number }
interface State { revision: number; enabled: boolean; listenerPort: number; clients: Client[];
  available: { name: string; description: string }[]; audit: Audit[] }
const session = useSessionStore();
const dialog = useDialogStore();
const state = ref<State | null>(null);
const busy = ref(false);
const error = ref('');
const editing = ref(false);
const editRevision = ref(0);
const form = reactive({ id: '', title: '', expiresAt: 0, scopes: [] as string[] });
const available = computed(() => session.connected && session.isOwner);
const labels: Record<string, string> = {
  nyabula_time: '设备时间', nyabula_system: '系统与内存状态', nyabula_timers: '全部计时器',
  nyabula_tasks: '全部个人待办', nyabula_calendar: '全部个人日程',
  nyabula_chat: '独立 AI 对话（使用设备模型账户）',
  nyabula_run: '查询该客户端的对话结果', nyabula_cancel: '取消该客户端的对话',
};
const canSave = computed(() => available.value && !busy.value && !!form.title.trim() &&
  new TextEncoder().encode(form.title).length <= 96 && form.expiresAt > Date.now());

async function request(action: string, data: Record<string, unknown> = {}): Promise<void> {
  if (!available.value || busy.value) return;
  const client = session.client;
  busy.value = true;
  try {
    const result = await session.request('agent.mcp.in.' + action, data);
    if (client !== session.client) return;
    if (!Array.isArray(result.clients) || !Array.isArray(result.available) ||
        !Array.isArray(result.audit) || typeof result.revision !== 'number' ||
        typeof result.enabled !== 'boolean' || typeof result.listenerPort !== 'number')
      throw new Error('设备返回了无效的入站清单');
    state.value = result as unknown as State; error.value = '';
    if (action === 'save') editing.value = false;
  } catch (cause) {
    if (client === session.client) error.value = String(cause) + '。请刷新核对设备状态，不自动重复提交。';
  } finally { busy.value = false; }
}
function open(client: Client): void {
  editRevision.value = state.value?.revision ?? 0;
  Object.assign(form, { ...client, scopes: [...client.scopes] });
  editing.value = true;
}
function scope(name: string, selected: boolean): void {
  form.scopes = selected ? [...form.scopes, name] : form.scopes.filter(item => item !== name);
}
async function save(): Promise<void> {
  if (!canSave.value) return;
  const client = session.client;
  const changes = { ...form, scopes: [...form.scopes], revision: editRevision.value };
  if (!await dialog.confirm('来访客户端将能直接读取所选范围内的全部数据，不经过逐次审批。若选择独立 AI 对话，将使用设备配置的模型账户，可能产生费用；模型可读取所选数据。会话与主人及其他客户端隔离，写操作、记忆、Skills、Shell、审批与外连管理不开放。',
    { title: '确认来访读取范围', confirmText: '保存读取范围' }) || client !== session.client) return;
  await request('save', changes);
}
async function enable(): Promise<void> {
  if (!state.value) return;
  const client = session.client;
  const revision = state.value.revision;
  const enabled = !state.value.enabled;
  if (enabled && !await dialog.confirm('仅允许持有未过期独立凭据的客户端使用已授权能力。对话授权会使用设备模型账户。端点还需在设备本地启动，仅监听回环地址；远程访问必须通过可信隧道。',
    { title: '允许 MCP 来访', confirmText: '允许来访' })) return;
  if (client !== session.client) return;
  await request('enable', { revision, enabled });
}
async function remove(entry: Client): Promise<void> {
  const client = session.client;
  const revision = state.value?.revision;
  if (!await dialog.confirm('删除这个客户端的凭据及全部授权，之后的请求将被拒绝。已读取的数据无法收回。',
    { title: '撤销来访客户端', danger: true, confirmText: '撤销客户端' }) || client !== session.client) return;
  await request('delete', { id: entry.id, revision });
}
watch(() => session.client, () => {
  state.value = null; editing.value = false; error.value = '';
  void request('list');
}, { immediate: true });
watch(() => session.connected, value => { if (value) void request('list'); });
</script>

<template>
  <section class="mcp-inbound">
    <h2>MCP · 允许外部访问</h2>
    <p>与上方外连完全独立。按客户端授权只读工具或独立 AI 对话；不开放写操作、记忆、Skills、Shell 或审批权限。AI 对话使用设备模型账户，可能产生费用。</p>
    <p v-if="!available" role="status">连接设备并以主人身份管理来访权限。</p>
    <p v-if="error" role="alert" class="error">{{ error }}</p>
    <div class="actions">
      <MdButton :disabled="!available || !state || busy" @click="enable">{{ state?.enabled ? '紧急关闭来访' : '允许 MCP 来访' }}</MdButton>
      <MdButton variant="text" :disabled="!available || busy" @click="request('list')">刷新来访清单</MdButton>
    </div>
    <template v-if="state">
      <p role="status">授权策略：{{ state.enabled ? '允许持有效凭据来访' : '已关闭，全部来访被拒绝' }}。
        监听入口：{{ state.listenerPort ? '127.0.0.1:' + state.listenerPort + '/mcp' : '未启动' }}。</p>
      <details class="setup">
        <summary>首次配对与轮换 · 需要设备本地管理</summary>
        <p>网页不接收或显示令牌。通过本地 CLI 导入由可信系统安全随机源生成的 32 字节令牌（64 位小写十六进制），设备只保存 SHA-256 哈希。</p>
        <p>本地调用 agent.mcp.in.save，指定客户端 id、title、expiresAt、scopes、token 和最新 revision；到期时间不超过 30 天。轮换时使用相同 id 和新 token，旧令牌立即失效。</p>
        <p>启动命令：<code>nyabula_mcp 8766 &amp;</code>；停止监听：<code>nyabula_mcp stop</code>。只支持本机或可信隧道，不是公开 OAuth/TLS 服务，不可把此明文端口直接转发到公网。</p>
      </details>
      <p v-if="!state.clients.length">尚未配对来访客户端。</p>
      <article v-for="entry in state.clients" :key="entry.id" class="in-client">
        <h3>{{ entry.title }}</h3><code>{{ entry.id }}</code>
        <p>{{ entry.expiresAt <= Date.now() ? '已过期' : '到期' }}：{{ new Date(entry.expiresAt).toLocaleString() }}</p>
        <p>授权范围：{{ entry.scopes.length ? entry.scopes.map(item => labels[item] ?? item).join('、') : '无，所有工具不可用' }}</p>
        <div class="actions">
          <MdButton variant="outlined" :disabled="!available || busy" @click="open(entry)">编辑读取范围</MdButton>
          <MdButton variant="text" :disabled="!available || busy" @click="remove(entry)">撤销客户端</MdButton>
        </div>
      </article>
      <form v-if="editing" class="in-editor" @submit.prevent="save">
        <h3>来访客户端 · {{ form.id }}</h3>
        <MdTextField v-model="form.title" label="来访客户端名称" :disabled="busy" />
        <fieldset :disabled="busy">
          <legend>可使用的能力与数据范围</legend>
          <label v-for="entry in state.available" :key="entry.name">
            <input type="checkbox" :checked="form.scopes.includes(entry.name)"
              @change="scope(entry.name, ($event.target as HTMLInputElement).checked)" />
            {{ labels[entry.name] ?? entry.description }}
          </label>
        </fieldset>
        <p>保存不延长有效期，也不更换凭据。已过期凭据须通过本地管理轮换。</p>
        <p>对话、结果查询、取消分别授权。对话仅使用该客户端的历史；关闭来访、撤销客户端或移除对话权限会在下一执行检查点取消正在进行的对话。</p>
        <div class="actions"><MdButton :disabled="!canSave">保存读取范围</MdButton>
          <MdButton type="button" variant="text" :disabled="busy" @click="editing = false">关闭范围编辑</MdButton></div>
      </form>
      <details class="audit">
        <summary>最近工具调用 · {{ state.audit.length }} 条（最多保留 32 条）</summary>
        <p v-if="!state.audit.length">暂无已认证的工具调用。</p>
        <article v-for="(event, index) in [...state.audit].reverse()" :key="index">
          {{ new Date(event.at).toLocaleString() }} · {{ event.client }} · {{ labels[event.tool] ?? event.tool }} ·
          {{ event.status === 0 ? '调用成功' : '已拒绝或调用失败（' + event.status + '）' }}
        </article>
        <p>仅记录已认证工具调用的来源、工具、时间和结果，不保存令牌、参数或读取内容。</p>
      </details>
    </template>
  </section>
</template>

<style scoped>
.mcp-inbound { margin-top: 28px; border-top: 1px solid var(--md-outline-variant); padding-top: 24px; min-width: 0; }
h2 { font: 600 22px var(--font-title); }
.actions { display: flex; flex-wrap: wrap; gap: 8px; }
.in-client { padding: 16px 0; border-bottom: 1px solid var(--md-outline-variant); }
.in-editor { display: flex; flex-direction: column; gap: 12px; padding: 20px 0; }
fieldset { border: 1px solid var(--md-outline-variant); border-radius: 12px; }
fieldset label { min-height: 44px; display: flex; gap: 12px; align-items: center; }
.setup, .audit { padding: 16px 0; }
summary { min-height: 44px; cursor: pointer; align-content: center; }
p, code, article { overflow-wrap: anywhere; }
.error { color: var(--md-error); }
</style>
