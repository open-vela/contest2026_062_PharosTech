<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue';
import { MdButton, MdTextField, UiIcon, useDialogStore } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import { useNyabotStore } from '../../stores/nyabot';
import { isLocalBackend } from '../../lib/deviceCompute';

interface Backend {
  index: number; host: string; path: string; port: string; model: string;
  priority: number; cost_tier: number; enabled: boolean; keySet: boolean;
  status: string; total_calls: number; total_failures: number; avg_latency_ms: number;
}
interface Router { profile: string; backends: Backend[] }
/* reloadKey: the 本机模型 card changed the router from its side; `changed` tells it the same. */
const props = defineProps<{ reloadKey?: number }>();
const emit = defineEmits<{ (e: 'changed'): void }>();
const session = useSessionStore();
const bot = useNyabotStore();
const dialog = useDialogStore();
const state = ref<Router | null>(null);
const busy = ref(false);
const error = ref('');
const editing = ref(false);
const form = reactive({ index: 0, host: '', path: '/v1/chat/completions', port: '443',
  model: '', key: '', enabled: true, priority: 0, cost_tier: 1 });
const available = computed(() => session.connected && session.isOwner && !!bot.status?.ready);
const canChange = computed(() => available.value && !busy.value && !bot.status?.activeRun);
const freeSlot = computed(() => [0, 1, 2, 3].find(index => !state.value?.backends.some(b => b.index === index)));
const profiles: Record<string, string> = { auto: '自动平衡', eco: '经济优先', premium: '高质量优先' };
const statuses: Record<string, string> = { ok: '可用', paused: '已暂停', degraded: '最近调用失败', backoff: '等待重试', disabled: '暂时不可用', recovering: '等待恢复' };

async function request(action: string, data: Record<string, unknown> = {}): Promise<void> {
  if (!available.value || busy.value) return;
  const client = session.client;
  busy.value = true;
  try {
    const result = await session.request('agent.config.router.' + action, data);
    if (client !== session.client) return;
    if (!Array.isArray(result.backends) || typeof result.profile !== 'string') throw new Error('设备返回的路由配置无效');
    state.value = result as unknown as Router;
    error.value = '';
    if (action === 'save') { editing.value = false; form.key = ''; }
    if (action === 'save' || action === 'delete') emit('changed');
    await bot.refresh();
  } catch (cause) { if (client === session.client) error.value = String(cause); }
  finally { busy.value = false; }
}
/* The on-device model is a backend the firmware writes itself (host nyabula.local):
 * shown here so the list is complete, managed in its own card. */
function showLocalCard(): void {
  document.getElementById('local-model')?.scrollIntoView({ behavior: 'smooth', block: 'start' });
}
function open(backend?: Backend): void {
  if (backend && isLocalBackend(backend.host)) return;
  Object.assign(form, backend ? { ...backend, key: '' } : { index: freeSlot.value ?? 0,
    host: '', path: '/v1/chat/completions', port: '443', model: '', key: '', enabled: true, priority: 0, cost_tier: 1 });
  editing.value = true;
}
function payload(): Record<string, unknown> {
  return { index: form.index, host: form.host.trim(), path: form.path.trim(), port: form.port.trim(),
    model: form.model.trim(), key: form.key, enabled: form.enabled,
    priority: Number(form.priority), cost_tier: Number(form.cost_tier) };
}
async function remove(backend: Backend): Promise<void> {
  if (isLocalBackend(backend.host)) return;
  if (!canChange.value || !await dialog.confirm('移除此后端及其密钥。其他后端和当前会话记录不受影响。',
    { title: '删除模型后端', danger: true, confirmText: '删除后端' })) return;
  await request('delete', { index: backend.index });
}
watch([() => session.client, () => bot.status?.ready], () => {
  state.value = null; editing.value = false; form.key = ''; void request('get');
}, { immediate: true });
watch(() => props.reloadKey, () => void request('get'));
</script>

<template>
  <section class="model-router">
    <h3>多模型路由</h3>
    <p>最多四个 OpenAI 兼容后端。启用后由设备按策略选择；没有启用的后端时使用上方单模型配置。费用档位是你的配置，不是实时价格。</p>
    <p v-if="error" role="alert" class="error">{{ error }}</p>
    <div class="actions">
      <MdButton :disabled="!canChange || freeSlot === undefined" @click="open()">添加模型后端</MdButton>
      <MdButton variant="text" :disabled="!available || busy" @click="request('get')">刷新路由状态</MdButton>
    </div>
    <template v-if="state">
      <label class="profile">路由策略
        <select aria-label="路由策略" :value="state.profile" :disabled="!canChange" @change="request('profile', { profile: ($event.target as HTMLSelectElement).value })">
          <option v-for="(label, value) in profiles" :key="value" :value="value">{{ label }}</option>
        </select>
      </label>
      <p v-if="!state.backends.length">尚未添加后端，当前使用单模型配置。</p>
      <template v-for="backend in state.backends" :key="backend.index">
      <article v-if="isLocalBackend(backend.host)" class="backend local">
        <h4><span class="local-icon"><UiIcon name="cloud_off" :size="16" /></span>本机模型 · 槽位 {{ backend.index + 1 }}
          <span class="tag info">离线 · 设备内置</span></h4>
        <p class="mono">{{ backend.model }}</p>
        <p>{{ statuses[backend.status] ?? backend.status }} · 无需密钥 · 优先级 {{ backend.priority }} · 费用档位 {{ backend.cost_tier }}（免费）</p>
        <p>成功 {{ backend.total_calls }} · 失败 {{ backend.total_failures }} · 平滑延迟 {{ backend.avg_latency_ms }} ms（本次运行统计）</p>
        <div class="actions">
          <MdButton variant="text" @click="showLocalCard">在「本机模型」卡片中管理</MdButton>
        </div>
      </article>
      <article v-else class="backend">
        <h4>{{ backend.model }} · 槽位 {{ backend.index + 1 }}</h4>
        <p>{{ backend.host }}:{{ backend.port }}{{ backend.path }}</p>
        <p>{{ statuses[backend.status] ?? backend.status }} · {{ backend.keySet ? '密钥已设置' : '未设置密钥' }} · 优先级 {{ backend.priority }} · 费用档位 {{ backend.cost_tier }}</p>
        <p>成功 {{ backend.total_calls }} · 失败 {{ backend.total_failures }} · 平滑延迟 {{ backend.avg_latency_ms }} ms（本次运行统计）</p>
        <div class="actions">
          <MdButton variant="outlined" :disabled="!canChange" @click="open(backend)">编辑模型后端</MdButton>
          <MdButton variant="text" :disabled="!canChange" @click="remove(backend)">删除模型后端</MdButton>
        </div>
      </article>
      </template>
    </template>
    <form v-if="editing" class="router-editor" @submit.prevent="request('save', payload())">
      <h4>模型后端 · 槽位 {{ form.index + 1 }}</h4>
      <MdTextField v-model="form.host" label="后端主机名" placeholder="不含 https://" :disabled="!canChange" />
      <MdTextField v-model="form.port" label="后端端口" :disabled="!canChange" />
      <MdTextField v-model="form.path" label="后端请求路径" :disabled="!canChange" />
      <MdTextField v-model="form.model" label="后端模型 ID" :disabled="!canChange" />
      <MdTextField v-model="form.key" label="后端密钥" type="password" autocomplete="off" placeholder="留空保留已保存密钥" :disabled="!canChange" />
      <label>优先级（数值越小越优先）<input v-model.number="form.priority" type="number" min="0" max="100" :disabled="!canChange" /></label>
      <label>费用档位<select v-model.number="form.cost_tier" :disabled="!canChange">
        <option :value="0">0 · 免费／本地</option><option :value="1">1 · 低</option>
        <option :value="2">2 · 中</option><option :value="3">3 · 高</option>
      </select></label>
      <label><input v-model="form.enabled" type="checkbox" :disabled="!canChange" />启用这个后端</label>
      <div class="actions">
        <MdButton :disabled="!canChange || !form.host.trim() || !form.model.trim()">保存模型后端</MdButton>
        <MdButton type="button" variant="text" :disabled="busy" @click="editing = false; form.key = ''">关闭后端编辑</MdButton>
      </div>
    </form>
    <p v-if="bot.status?.activeRun">对话正在运行，完成后可修改模型配置。</p>
  </section>
</template>

<style scoped>
.model-router { margin-top: 28px; padding-top: 20px; border-top: 1px solid var(--md-outline-variant); }
.actions { display: flex; gap: 8px; flex-wrap: wrap; }
.backend h4 { display: flex; align-items: center; flex-wrap: wrap; gap: 6px 8px; }
.local-icon { width: 26px; height: 26px; flex: none; border-radius: 8px; display: grid; place-items: center; background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.backend { padding: 12px 0; border-bottom: 1px solid var(--md-outline-variant); overflow-wrap: anywhere; }
.router-editor { display: flex; flex-direction: column; gap: 16px; padding-top: 20px; }
label { display: flex; align-items: center; flex-wrap: wrap; gap: 12px; min-height: 44px; }
select, input[type=number] { min-height: 44px; border: 1px solid var(--md-outline); border-radius: 8px; padding: 8px; background: var(--md-surface); color: var(--md-on-surface); }
.profile { margin: 16px 0; }
.error { color: var(--md-error); }
</style>
