<script setup lang="ts">
import { computed, onBeforeUnmount, reactive, ref, watch } from 'vue';
import { MdButton, MdTextField } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import { useNyabotStore } from '../../stores/nyabot';

interface MqttState { broker: string; clientId: string; topicIn: string; topicOut: string; username: string; passwordSet: boolean; requested: boolean; running: boolean; connected: boolean; lastError: number }
const session = useSessionStore();
const bot = useNyabotStore();
const state = ref<MqttState | null>(null);
const form = reactive({ broker: '', clientId: 'nyabula', topicIn: 'nyabula/in', topicOut: 'nyabula/out', username: '', password: '' });
const busy = ref(false);
const error = ref('');
const message = ref('');
const supported = computed(() => bot.capabilities.some(item => item.id === 'mqtt' && item.compiled));
const available = computed(() => supported.value && session.connected && session.isOwner && bot.status?.ready);
const fields = ['broker', 'clientId', 'topicIn', 'topicOut', 'username'] as const;
const dirty = computed(() => !!state.value && (form.password !== '' || fields.some(key => form[key] !== state.value?.[key])));
const stopped = computed(() => !!state.value && !state.value.running && !state.value.requested);
const status = computed(() => !state.value ? '尚未读取' : state.value.connected ? '已连接 Broker' : state.value.running ? '连接中／重连中' : state.value.requested ? '正在启动' : '已停止');
function restore(): void { if (state.value) for (const key of fields) form[key] = state.value[key]; form.password = ''; }
async function request(action: 'get' | 'save' | 'start' | 'stop', populate = false): Promise<void> {
  if (!available.value || busy.value) return;
  const client = session.client;
  const wasDirty = dirty.value;
  busy.value = true;
  try {
    const { password, ...config } = form;
    const data = action === 'save' ? { ...config, ...(password ? { password } : {}) } : {};
    const result = await session.request(`agent.channels.mqtt.${action}`, data);
    if (client !== session.client) return;
    state.value = result as unknown as MqttState;
    if (populate || action === 'save' || (!wasDirty && action === 'get')) restore();
    error.value = '';
    if (action !== 'get') message.value = action === 'save' ? '配置已保存。请手动启动通道。' : action === 'start' ? '启动请求已提交。' : '停止请求已提交。';
  } catch (cause) { if (client === session.client) error.value = String(cause); }
  finally { busy.value = false; }
}
watch([() => session.client, available], () => { state.value = null; void request('get', true); }, { immediate: true });
const timer = setInterval(() => { if (state.value?.running || state.value?.requested) void request('get'); }, 2000);
onBeforeUnmount(() => clearInterval(timer));
</script>

<template>
  <section class="mqtt-panel">
    <h2>MQTT 消息渠道</h2>
    <p>指定 Topic 中的消息会以主人配置运行 Nyabot，结果回传到输出 Topic；执行记录和审批仍在任务页。仅连接你控制的 Broker，当前使用明文 MQTT。</p>
    <p v-if="!supported">当前固件未编译 MQTT 通道。</p>
    <p v-if="error" class="error" role="alert">{{ error }}</p>
    <p role="status">{{ status }}<span v-if="state?.lastError"> · 错误 {{ state.lastError }}</span></p>
    <form @submit.prevent="request('save')">
      <MdTextField v-model="form.broker" label="MQTT Broker" placeholder="主机名:1883" :disabled="!available || !stopped || busy" />
      <MdTextField v-model="form.clientId" label="MQTT 客户端 ID" :disabled="!available || !stopped || busy" />
      <MdTextField v-model="form.topicIn" label="接收 Topic" :disabled="!available || !stopped || busy" />
      <MdTextField v-model="form.topicOut" label="回复 Topic" :disabled="!available || !stopped || busy" />
      <MdTextField v-model="form.username" label="MQTT 用户名" :disabled="!available || !stopped || busy" />
      <MdTextField v-model="form.password" label="MQTT 密码" type="password" autocomplete="off" :placeholder="state?.passwordSet ? '已设置，留空保留' : '可选'" :disabled="!available || !stopped || busy" />
      <div class="actions">
        <MdButton :disabled="!available || !stopped || !dirty || busy || !form.broker || !form.clientId || !form.topicIn || !form.topicOut">保存 MQTT 配置</MdButton>
        <MdButton type="button" variant="tonal" :disabled="!available || !stopped || dirty || busy || !state?.broker" @click="request('start')">启动 MQTT</MdButton>
        <MdButton type="button" variant="tonal" :disabled="!available || stopped || busy" @click="request('stop')">停止 MQTT</MdButton>
        <MdButton type="button" variant="text" :disabled="!available || busy" @click="request('get', true)">重新读取 MQTT</MdButton>
      </div>
      <p>{{ dirty ? '配置尚未保存' : message }}</p>
    </form>
    <details><summary>消息格式与使用</summary>
      <pre>{"chat_id":"owner","request_id":"message-001","content":"你好"}</pre>
      <p>chat_id 使用字母、数字、连字符或下划线，最多 48 字符。request_id 可省略；同一 ID 重发不会重复调用模型。纯文本消息进入默认会话。设备重启后配置保留，通道需重新启动。</p>
    </details>
  </section>
</template>

<style scoped>
.mqtt-panel { max-width: 760px; width: 100%; margin: 0 auto; line-height: 1.6; }
form { display: flex; flex-direction: column; gap: 16px; }
.actions { display: flex; flex-wrap: wrap; gap: 8px; }
pre { white-space: pre-wrap; overflow-wrap: anywhere; padding: 12px; background: var(--md-surface-container); border-radius: 8px; }
details { margin-top: 24px; }
summary { cursor: pointer; min-height: 44px; }
.error { color: var(--md-error); }
</style>
