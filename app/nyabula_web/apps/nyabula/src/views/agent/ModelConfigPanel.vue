<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue';
import { MdButton, MdTextField } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import { useNyabotStore } from '../../stores/nyabot';
import ModelRouterPanel from './ModelRouterPanel.vue';
import PersonaPanel from './PersonaPanel.vue';

interface ModelConfig { host: string; path: string; port: string; model: string; keySet: boolean; canSave: boolean }
const session = useSessionStore();
const bot = useNyabotStore();
const saved = ref<ModelConfig | null>(null);
const form = reactive({ host: '', path: '/v1/chat/completions', port: '443', model: '', key: '' });
const busy = ref(false);
const error = ref('');
const applied = ref(false);
const dirty = computed(() => !!saved.value && (form.key !== '' || ['host', 'path', 'port', 'model'].some(
  key => form[key as keyof typeof form] !== saved.value?.[key as keyof ModelConfig])));
function discard(): void {
  if (saved.value) Object.assign(form, { host: saved.value.host, path: saved.value.path,
    port: saved.value.port, model: saved.value.model, key: '' });
  applied.value = false;
}
async function load(): Promise<void> {
  if (!session.connected || !session.isOwner || !bot.status?.ready || busy.value) return;
  const client = session.client;
  busy.value = true;
  try {
    const data = await session.request('agent.config.get', {});
    if (client !== session.client) return;
    saved.value = data as unknown as ModelConfig;
    discard(); error.value = '';
  } catch (cause) { error.value = cause instanceof Error ? cause.message : String(cause); }
  finally { busy.value = false; }
}
async function save(): Promise<void> {
  if (!saved.value?.canSave || busy.value || !session.isOwner) return;
  busy.value = true;
  try {
    const data = await session.request('agent.config.set', { ...form });
    saved.value = data as unknown as ModelConfig;
    form.key = ''; applied.value = true; error.value = '';
    await bot.refresh();
  } catch (cause) { error.value = cause instanceof Error ? cause.message : String(cause); }
  finally { busy.value = false; }
}
watch([() => session.client, () => bot.status?.ready], () => { if (!dirty.value) void load(); }, { immediate: true });
</script>

<template>
  <section class="model-config">
    <h2>模型与路由</h2>
    <p>{{ saved?.keySet ? '密钥已设置，页面不会读取原文。' : '尚未设置模型密钥。未配置时不会请求模型。' }}</p>
    <p class="notice">请仅在可信局域网或隧道中配置密钥。原生 WebSocket 连接未加密；页面不会回读已保存密钥。运行对话可能产生模型费用。</p>
    <p v-if="error" class="error" role="alert">{{ error }}</p>
    <form @submit.prevent="save">
      <MdTextField v-model="form.host" label="模型主机名" placeholder="不含 https://" :disabled="!saved?.canSave || busy" />
      <div class="address">
        <MdTextField v-model="form.port" label="TLS 端口" :disabled="!saved?.canSave || busy" />
        <MdTextField v-model="form.path" label="请求路径" :disabled="!saved?.canSave || busy" />
      </div>
      <MdTextField v-model="form.model" label="模型 ID" :disabled="!saved?.canSave || busy" />
      <MdTextField v-model="form.key" label="替换密钥" type="password" autocomplete="off" :placeholder="saved?.keySet ? '留空保留现有密钥' : '尚未设置'" :disabled="!saved?.canSave || busy" />
      <div class="actions">
        <MdButton :disabled="!saved?.canSave || !dirty || busy">保存到设备</MdButton>
        <MdButton type="button" variant="text" :disabled="!dirty || busy" @click="discard">撤销未保存修改</MdButton>
        <MdButton type="button" variant="text" :disabled="dirty || busy || !session.connected" @click="load">刷新</MdButton>
      </div>
      <p role="status">{{ busy ? '处理中' : dirty ? '未保存' : applied ? '已应用' : '设备配置' }}</p>
    </form>
    <ModelRouterPanel />
    <PersonaPanel />
  </section>
</template>

<style scoped>
.model-config { max-width: 680px; line-height: 1.6; }
h2 { font: 600 22px var(--font-title); }
form { display: flex; flex-direction: column; gap: 16px; }
.address { display: grid; grid-template-columns: 120px 1fr; gap: 12px; }
.actions { display: flex; flex-wrap: wrap; gap: 8px; }
.notice { padding: 12px; background: var(--md-surface-container); border-radius: 12px; }
.error { color: var(--md-error); }
</style>
