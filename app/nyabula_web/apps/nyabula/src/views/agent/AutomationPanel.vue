<script setup lang="ts">
import { computed, onBeforeUnmount, reactive, ref, watch } from 'vue';
import { MdButton, MdTextField, useDialogStore } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import { useNyabotStore } from '../../stores/nyabot';
import { RUN_LABELS } from './agent.logic';
const emit = defineEmits<{ open: [conversation: string] }>();
interface Rule { id: string; title: string; prompt: string; kind: 'at' | 'every' | 'heartbeat'; intervalSeconds: number; atMs: number; enabled: boolean; pending: boolean; nextAt: number; state: string; error: number; lastRun?: string }
const session = useSessionStore();
const bot = useNyabotStore();
const dialog = useDialogStore();
const items = ref<Rule[]>([]);
const revision = ref(0), editRevision = ref(0);
const busy = ref(false), editing = ref(false), existing = ref(false);
const loading = ref(false);
const loaded = ref(false);
let sequence = 0;
const error = ref('');
const form = reactive({ id: '', title: '', prompt: '', kind: 'every', intervalSeconds: 3600, localTime: '' });
const available = computed(() => session.connected && session.isOwner);
const bytes = (value: string) => new TextEncoder().encode(value).length;
const canSave = computed(() => available.value && !busy.value && /^[a-z0-9_-]{1,24}$/.test(form.id) &&
  bytes(form.title) > 0 && bytes(form.title) <= 96 && bytes(form.prompt) > 0 && bytes(form.prompt) <= 1024 &&
  (form.kind === 'at' ? Number.isFinite(new Date(form.localTime).getTime()) : Number.isInteger(Number(form.intervalSeconds)) && Number(form.intervalSeconds) >= 30 && Number(form.intervalSeconds) <= 2592000));
const stateNames: Record<string,string> = { paused: '已暂停', scheduled: '等待触发', pending: '等待设备执行', submitted: '已创建执行任务', blocked: '已暂停，需要处理错误', missed: '已跳过离线错过的执行' };
const errors: Record<number,string> = { [-61]: '尚未配置模型', [-16]: 'Nyabot 正忙，稍后派发', [-11]: 'Nyabot 正在启动', [-28]: '会话存储不足', [-114]: '原请求已删除，不会重放' };
function apply(data: Record<string,unknown>): void {
  if (!Array.isArray(data.items) || typeof data.revision !== 'number') throw new Error('无效的自动化清单');
  items.value = data.items as Rule[]; revision.value = data.revision; loaded.value = true;
}
async function load(): Promise<void> {
  if (!available.value || busy.value || loading.value) return;
  const client = session.client; const current = ++sequence; loading.value = true;
  try { const data = await session.request('agent.automation.list', {}); if (client === session.client && current === sequence) { apply(data); error.value = ''; } }
  catch(cause) { if (client === session.client && current === sequence) error.value = String(cause); }
  finally { loading.value = false; }
}
function edit(rule?: Rule): void {
  editRevision.value = revision.value; existing.value = !!rule;
  Object.assign(form, rule ? { id: rule.id, title: rule.title, prompt: rule.prompt, kind: rule.kind,
    intervalSeconds: rule.intervalSeconds || 3600, localTime: rule.atMs ? new Date(rule.atMs - new Date(rule.atMs).getTimezoneOffset()*60000).toISOString().slice(0,16) : '' }
    : { id: '', title: '', prompt: '', kind: 'every', intervalSeconds: 3600, localTime: '' });
  editing.value = true;
}
async function change(topic: string, data: Record<string,unknown>): Promise<void> {
  if (!available.value || busy.value) return;
  ++sequence;
  const client = session.client; busy.value = true;
  try {
    const result = await session.request(topic, { ...data, revision: topic === 'agent.automation.save' ? editRevision.value : revision.value });
    if (client !== session.client) return;
    apply(result); error.value = ''; if (topic.endsWith('.save')) editing.value = false;
    await bot.refresh();
  } catch(cause) { if (client === session.client) error.value = String(cause) + '。版本冲突时请保留草稿，重新打开规则后核对。'; }
  finally { busy.value = false; }
}
async function toggle(rule: Rule): Promise<void> {
  if (!rule.enabled && !await dialog.confirm('设备会按此规则调用模型，可能产生模型费用。写操作仍需逐次审批；关闭页面不会停止执行。', { title: '启用自动化', confirmText: '启用' })) return;
  await change('agent.automation.enable', { id: rule.id, enabled: !rule.enabled });
}
async function run(rule: Rule): Promise<void> {
  if (await dialog.confirm('将向设备提交一次真实执行，可能调用模型。不会自动批准任何写操作。', { title: '立即运行', confirmText: '运行一次' }))
    await change('agent.automation.run', { id: rule.id });
}
async function remove(rule: Rule): Promise<void> {
  if (await dialog.confirm('删除调度规则，不删除执行记录，也不取消已经创建的任务。', { title: '删除自动化', danger: true, confirmText: '删除' }))
    await change('agent.automation.delete', { id: rule.id });
}
function timestamp(value: number): string { return value > 0 ? new Date(value).toLocaleString() : '未安排'; }
function runState(rule: Rule): string { const run = bot.runs.find(r => r.id === rule.lastRun); return run ? RUN_LABELS[run.state] : stateNames[rule.state] ?? rule.state; }
watch(() => session.client, () => { items.value = []; loaded.value = false; editing.value = false; error.value = ''; void load(); }, { immediate: true });
watch(() => session.connected, connected => { if (connected) void load(); });
const timer = setInterval(() => { if (!editing.value) void load(); }, 3000);
onBeforeUnmount(() => clearInterval(timer));
</script>

<template>
  <section class="automation-panel">
    <h2>自动化与主动检查</h2>
    <p>规则保存在设备。一次性、周期任务和主动检查都进入统一执行记录。重启时不补跑未派发的过期任务，周期任务重新计时；已确认派发的请求只核对原标识，不重复执行。</p>
    <p v-if="!available">连接设备并以主人身份管理自动化。</p>
    <p v-if="error" role="alert" class="error">{{ error }}</p>
    <div class="actions"><MdButton :disabled="!available || !loaded || busy" @click="edit()">新建自动化</MdButton><MdButton variant="text" :disabled="!available || busy || loading" @click="load">刷新规则</MdButton></div>
    <form v-if="editing" @submit.prevent="canSave && change('agent.automation.save', { id: form.id, title: form.title, prompt: form.prompt, kind: form.kind, intervalSeconds: Number(form.intervalSeconds), atMs: form.kind === 'at' ? new Date(form.localTime).getTime() : 0 })">
      <h3>{{ existing ? '编辑规则' : '创建规则' }}</h3>
      <MdTextField v-model="form.id" label="规则 ID" :disabled="existing || busy" placeholder="小写字母、数字、短横线，最多 24 字符" />
      <MdTextField v-model="form.title" label="规则名称" :disabled="busy" />
      <label>触发方式<select v-model="form.kind" aria-label="触发方式" :disabled="busy"><option value="every">周期任务</option><option value="at">指定时间一次</option><option value="heartbeat">主动检查</option></select></label>
      <label v-if="form.kind === 'at'">触发时间（当前浏览器时区）<input v-model="form.localTime" aria-label="触发时间" type="datetime-local" :disabled="busy" /></label>
      <label v-else>间隔秒数（30～2592000）<input v-model.number="form.intervalSeconds" aria-label="间隔秒数" type="number" min="30" max="2592000" :disabled="busy" /></label>
      <label>执行要求<textarea v-model="form.prompt" aria-label="执行要求" rows="5" :disabled="busy" /></label>
      <p>{{ bytes(form.prompt) }} / 1024 字节。{{ form.kind === 'heartbeat' ? '没有可行动变化时，模型应返回 HEARTBEAT_OK；仍保留执行记录。' : '可用工具和逐次审批规则与聊天一致。' }}</p>
      <div class="actions"><MdButton :disabled="!canSave">保存为暂停</MdButton><MdButton type="button" variant="text" @click="editing = false">关闭编辑器</MdButton></div>
    </form>
    <p v-if="!items.length && !busy">尚无自动化规则。</p>
    <article v-for="rule in items" :key="rule.id" class="rule">
      <h3>{{ rule.title }}</h3><p>{{ rule.enabled ? '已启用' : '已暂停' }} · {{ runState(rule) }}</p>
      <p>{{ rule.prompt }}</p><p>下次触发：{{ timestamp(rule.nextAt) }} · {{ rule.kind === 'at' ? '一次性' : rule.kind === 'heartbeat' ? '主动检查' : '周期任务' }}</p>
      <p v-if="rule.error" class="error">{{ errors[rule.error] ?? `设备错误 ${rule.error}` }}</p>
      <div class="actions"><MdButton variant="text" :disabled="!available || busy" @click="edit(rule)">编辑规则</MdButton><MdButton variant="tonal" :disabled="!available || busy" @click="toggle(rule)">{{ rule.enabled ? '暂停' : '启用' }}</MdButton><MdButton variant="text" :disabled="!available || busy || rule.pending" @click="run(rule)">立即运行</MdButton><MdButton v-if="rule.lastRun" variant="text" @click="emit('open', 'automation-' + rule.id)">查看执行</MdButton><MdButton variant="text" :disabled="!available || busy" @click="remove(rule)">删除规则</MdButton></div>
    </article>
  </section>
</template>

<style scoped>
.automation-panel { max-width: 960px; width: 100%; margin: 0 auto; line-height: 1.6; }
h2 { font: 600 22px var(--font-title); }
.actions { display: flex; flex-wrap: wrap; gap: 8px; }
form { display: flex; flex-direction: column; gap: 12px; padding: 20px 0; }
input, select, textarea { display: block; width: 100%; box-sizing: border-box; background: var(--md-surface-container); color: var(--md-on-surface); padding: 12px; border: 1px solid var(--md-outline); border-radius: 8px; font: 15px/1.6 var(--font-body); }
.rule { border-bottom: 1px solid var(--md-outline-variant); padding: 18px 0; overflow-wrap: anywhere; }
.rule h3 { margin: 0; }.rule p { margin: 8px 0; }.error { color: var(--md-error); }
</style>
