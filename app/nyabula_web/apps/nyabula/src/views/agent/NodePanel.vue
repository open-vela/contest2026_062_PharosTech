<script setup lang="ts">
import { computed, onBeforeUnmount, reactive, ref, watch } from 'vue';
import { MdButton, MdTextField } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import { useNyabotStore } from '../../stores/nyabot';
interface NodeState { host: string; port: string; tls: boolean; tokenSet: boolean; deviceTokenSet: boolean; deviceId: string; requested: boolean; running: boolean; connected: boolean; ready: boolean; protocol: number; lastError: number; gatewayError: string; commands: string[] }
const session = useSessionStore();
const bot = useNyabotStore();
const state = ref<NodeState | null>(null);
const form = reactive({ host: '', port: '18789', tls: false, token: '' });
const clear = ref(false);
const busy = ref(false);
const error = ref('');
const message = ref('');
const available = computed(() => session.connected && session.isOwner && bot.status?.ready && bot.capabilities.some(item => item.id === 'openclaw-node' && item.compiled));
const stopped = computed(() => !!state.value && !state.value.running && !state.value.requested);
const dirty = computed(() => !!state.value && (form.host !== state.value.host || form.port !== state.value.port || form.tls !== state.value.tls || form.token !== '' || clear.value));
const status = computed(() => !state.value ? '尚未读取' : state.value.ready ? `节点已接入 · 协议 ${state.value.protocol}` : state.value.running ? !state.value.requested ? '节点正在停止' : state.value.connected ? '传输已连接，等待认证／配对' : '节点连接中／重连中' : state.value.requested ? '节点正在启动' : '节点已停止');
async function request(action: 'get' | 'save' | 'start' | 'stop', populate = false): Promise<void> {
  if (!available.value || busy.value) return;
  const client = session.client;
  const wasDirty = dirty.value;
  busy.value = true;
  try {
    const data = action === 'save' ? { host: form.host, port: form.port, tls: form.tls,
      ...(clear.value ? { token: '' } : form.token ? { token: form.token } : {}) } : {};
    const result = await session.request(`agent.node.${action}`, data);
    if (client !== session.client) return;
    state.value = result as unknown as NodeState;
    if (populate || action === 'save' || action === 'get' && !wasDirty) { form.host = state.value.host; form.port = state.value.port; form.tls = state.value.tls; form.token = ''; clear.value = false; }
    error.value = '';
    if (action === 'save') message.value = 'OpenClaw 配置已保存。';
    if (action === 'start') message.value = '等待 Gateway 挑战、身份认证与节点接纳。';
    if (action === 'stop') message.value = '停止连接，保留设备身份和 Core 运行记录。';
  } catch (cause) { if (client === session.client) error.value = String(cause); }
  finally { busy.value = false; }
}
watch([() => session.client, available], () => { state.value = null; void request('get', true); }, { immediate: true });
const timer = setInterval(() => { if (state.value?.running || state.value?.requested) void request('get'); }, 2000);
onBeforeUnmount(() => clearInterval(timer));
</script>
<template>
  <section class="node-panel">
    <h2>OpenClaw 节点</h2>
    <p>让 Nyabula 作为节点连接你管理的 Gateway。远端可查询 Core 状态、发起 Nyabot 对话、查询或取消运行；模型写操作仍在本机任务页确认。</p>
    <p>首次连接可能需要在 Gateway 批准设备配对，并允许下方自定义命令。这里只接入节点客户端，不把设备伪装成完整 Gateway。</p>
    <p v-if="!available">需要支持 OpenClaw 节点的固件和主人身份。</p>
    <p v-if="error" class="error" role="alert">{{ error }}</p>
    <p role="status">{{ status }}</p>
    <p v-if="state?.gatewayError" class="error" role="alert">Gateway 返回：{{ state.gatewayError }}。请检查配对、凭据和 Gateway 配置。</p>
    <p v-if="state?.lastError" class="error">节点错误 {{ state.lastError }}</p>
    <form @submit.prevent="request('save')">
      <MdTextField v-model="form.host" label="Gateway 主机" placeholder="IP 或主机名，不含 ws://" :disabled="!available || !stopped || busy" />
      <MdTextField v-model="form.port" label="Gateway 端口" :disabled="!available || !stopped || busy" />
      <label><input v-model="form.tls" type="checkbox" aria-label="Gateway 使用 TLS" :disabled="!available || !stopped || busy" />Gateway 使用 TLS（wss）</label>
      <p v-if="!form.tls">明文 ws 仅用于可信局域网或本地隧道。</p>
      <MdTextField v-model="form.token" label="Gateway Token" type="password" autocomplete="off" :placeholder="state?.tokenSet ? '已设置，留空保留' : state?.deviceTokenSet ? '已有设备令牌，可留空' : '按 Gateway 认证配置填写'" :disabled="!available || !stopped || busy || clear" />
      <label><input v-model="clear" type="checkbox" aria-label="清除 Gateway Token" :disabled="!available || !stopped || busy" />清除共享 Token，已有设备令牌仍可用于重连</label>
      <div class="actions">
        <MdButton :disabled="!available || !stopped || busy || !dirty || !form.host">保存 OpenClaw 配置</MdButton>
        <MdButton type="button" variant="tonal" :disabled="!available || !stopped || busy || dirty || !state?.host" @click="request('start')">启动 OpenClaw 节点</MdButton>
        <MdButton type="button" variant="tonal" :disabled="!available || stopped || busy" @click="request('stop')">停止 OpenClaw 节点</MdButton>
        <MdButton type="button" variant="text" :disabled="!available || busy" @click="request('get', true)">重新读取节点配置</MdButton>
      </div>
      <p>{{ dirty ? '节点配置尚未保存' : message }}</p>
    </form>
    <h3>设备身份与命令</h3>
    <p>{{ state?.deviceTokenSet ? 'Gateway 设备令牌已保存。' : '尚未保存 Gateway 设备令牌。' }}</p>
    <p>设备指纹：<code>{{ state?.deviceId || '首次连接时创建' }}</code></p>
    <ul><li v-for="command in state?.commands" :key="command"><code>{{ command }}</code></li></ul>
    <p>只有 Gateway 返回认证成功的 hello-ok，页面才显示“节点已接入”。</p>
  </section>
</template>
<style scoped>
.node-panel { max-width: 760px; width: 100%; margin: 0 auto; line-height: 1.6; }
form { display: flex; flex-direction: column; gap: 16px; }
.actions { display: flex; flex-wrap: wrap; gap: 8px; }
label:has(input[type="checkbox"]) { display: flex; align-items: center; gap: 8px; min-height: 44px; }
code { overflow-wrap: anywhere; }
.error { color: var(--md-error); }
</style>
