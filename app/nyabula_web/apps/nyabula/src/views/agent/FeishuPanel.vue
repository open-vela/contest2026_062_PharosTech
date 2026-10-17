<script setup lang="ts">
import { computed, onBeforeUnmount, reactive, ref, watch } from 'vue';
import { MdButton, MdTextField } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import { useNyabotStore } from '../../stores/nyabot';
interface FeishuState { appId: string; secretSet: boolean; requested: boolean; running: boolean; connected: boolean; lastError: number }
const session = useSessionStore();
const bot = useNyabotStore();
const state = ref<FeishuState | null>(null);
const form = reactive({ appId: '', secret: '' });
const clear = ref(false);
const busy = ref(false);
const error = ref('');
const message = ref('');
const available = computed(() => session.connected && session.isOwner && bot.status?.ready && bot.capabilities.some(item => item.id === 'feishu' && item.compiled));
const stopped = computed(() => !!state.value && !state.value.running && !state.value.requested);
const dirty = computed(() => !!state.value && (form.appId !== state.value.appId || form.secret !== '' || clear.value));
const status = computed(() => !state.value ? '尚未读取' : state.value.connected ? '飞书长连接已建立' : state.value.running ? state.value.requested ? '飞书连接中／重连中' : '飞书通道正在停止' : state.value.requested ? '飞书通道正在启动' : '飞书通道已停止');
async function request(action: 'get' | 'save' | 'start' | 'stop', populate = false): Promise<void> {
  if (!available.value || busy.value) return;
  const client = session.client;
  const wasDirty = dirty.value;
  busy.value = true;
  try {
    const data = action === 'save' ? { appId: form.appId, ...(clear.value ? { secret: '' } : form.secret ? { secret: form.secret } : {}) } : {};
    const result = await session.request(`agent.channels.feishu.${action}`, data);
    if (client !== session.client) return;
    state.value = result as unknown as FeishuState;
    if (populate || action === 'save' || action === 'get' && !wasDirty) { form.appId = state.value.appId; form.secret = ''; clear.value = false; }
    error.value = '';
    if (action === 'save') message.value = '飞书配置已保存。';
    if (action === 'start') message.value = '正在获取平台令牌并建立长连接。';
    if (action === 'stop') message.value = '等待当前网络请求退出，已有会话记录保留。';
  } catch (cause) { if (client === session.client) error.value = String(cause); }
  finally { busy.value = false; }
}
watch([() => session.client, available], () => { state.value = null; void request('get', true); }, { immediate: true });
const timer = setInterval(() => { if (state.value?.running || state.value?.requested) void request('get'); }, 2000);
onBeforeUnmount(() => clearInterval(timer));
</script>
<template>
  <section class="feishu-panel">
    <h2>飞书消息渠道</h2>
    <p>使用飞书应用机器人长连接接收文字消息，不需要公网回调服务器。消息使用设备的模型与主人资料；写操作仍在 Nyabot 任务页确认。</p>
    <p>请先在飞书应用中启用机器人、长连接事件订阅、消息接收与发送权限，并发布到相应可用范围。平台权限是否齐全须以真实账号联调为准。</p>
    <p v-if="!available">需要连接支持飞书通道的固件，并使用主人身份。</p>
    <p v-if="error" role="alert" class="error">{{ error }}</p>
    <p role="status">{{ status }}<span v-if="state?.lastError"> · 错误 {{ state.lastError }}</span></p>
    <form @submit.prevent="request('save')">
      <MdTextField v-model="form.appId" label="飞书 App ID" :disabled="!available || !stopped || busy" />
      <MdTextField v-model="form.secret" label="飞书 App Secret" type="password" autocomplete="off" :placeholder="state?.secretSet ? '已设置，留空保留' : '尚未设置'" :disabled="!available || !stopped || busy || clear" />
      <label><input v-model="clear" type="checkbox" aria-label="清除飞书密钥" :disabled="!available || !stopped || busy" />清除已保存密钥</label>
      <div class="actions">
        <MdButton :disabled="!available || !stopped || busy || !dirty || !form.appId">保存飞书配置</MdButton>
        <MdButton type="button" variant="tonal" :disabled="!available || !stopped || busy || dirty || !state?.secretSet" @click="request('start')">启动飞书通道</MdButton>
        <MdButton type="button" variant="tonal" :disabled="!available || stopped || busy" @click="request('stop')">停止飞书通道</MdButton>
        <MdButton type="button" variant="text" :disabled="!available || busy" @click="request('get', true)">重新读取飞书配置</MdButton>
      </div>
      <p>{{ dirty ? '飞书配置尚未保存' : message }}</p>
    </form>
  </section>
</template>
<style scoped>
.feishu-panel { max-width: 760px; width: 100%; margin: 0 auto; line-height: 1.6; }
form { display: flex; flex-direction: column; gap: 16px; }
.actions { display: flex; gap: 8px; flex-wrap: wrap; }
label:has(input[type="checkbox"]) { display: flex; align-items: center; gap: 8px; min-height: 44px; }
.error { color: var(--md-error); }
</style>
