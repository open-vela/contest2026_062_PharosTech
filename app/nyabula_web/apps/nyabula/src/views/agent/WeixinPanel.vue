<script setup lang="ts">
import { computed, onBeforeUnmount, reactive, ref, watch } from 'vue';
import qrcode from '../../vendor/qrcode-generator/qrcode.js';
import { MdButton, MdTextField } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import { useNyabotStore } from '../../stores/nyabot';

interface WeixinState { host: string; port: string; tokenSet: boolean; requested: boolean; running: boolean; connected: boolean; loginState: number; qrContent: string; lastError: number }
const session = useSessionStore();
const bot = useNyabotStore();
const state = ref<WeixinState | null>(null);
const form = reactive({ host: 'ilinkai.weixin.qq.com', port: '443', token: '' });
const clearToken = ref(false);
const busy = ref(false);
const error = ref('');
const message = ref('');
const qrImage = ref('');
qrcode.stringToBytes = qrcode.stringToBytesFuncs['UTF-8']!;
const supported = computed(() => bot.capabilities.some(item => item.id === 'weixin' && item.compiled));
const available = computed(() => supported.value && session.connected && session.isOwner && bot.status?.ready);
const stopped = computed(() => !!state.value && !state.value.running && !state.value.requested);
const dirty = computed(() => !!state.value && (form.host !== state.value.host || form.port !== state.value.port || form.token !== '' || clearToken.value));
const status = computed(() => !state.value ? '尚未读取' : state.value.connected ? '微信通道已连接' : state.value.running ? state.value.requested ? '微信连接中／重连中' : '微信通道正在停止' : state.value.requested ? '微信通道正在启动' : state.value.tokenSet ? '已登录，微信通道未启动' : '微信尚未登录');
const loginStatus = computed(() => state.value?.loginState === 1 ? '微信登录已确认' : state.value?.loginState === 2 ? '已扫码，请在微信确认' : state.value?.loginState === -3 ? '二维码已过期，请重新获取' : '请用微信扫描二维码');
watch(() => state.value?.qrContent, content => {
  qrImage.value = '';
  if (content) {
    try {
      const code = qrcode(0, 'M');
      code.addData(content); code.make();
      const image = code.createDataURL(6, 12);
      if (state.value?.qrContent === content) qrImage.value = image;
    } catch (cause) { error.value = String(cause); }
  }
});
async function request(action: string, populate = false): Promise<void> {
  if (!available.value || busy.value) return;
  const client = session.client;
  const wasDirty = dirty.value;
  busy.value = true;
  try {
    const data = action === 'save' ? { host: form.host, port: form.port,
      ...(clearToken.value ? { token: '' } : form.token ? { token: form.token } : {}) } : {};
    const result = await session.request(`agent.channels.weixin.${action}`, data);
    if (client !== session.client) return;
    state.value = result as unknown as WeixinState;
    if (populate || action === 'save' || action === 'login.poll' && state.value.loginState === 1 || !wasDirty && action === 'get') {
      form.host = state.value.host; form.port = state.value.port; form.token = ''; clearToken.value = false;
    }
    error.value = '';
    if (action === 'save') message.value = '微信配置已保存。';
    if (action === 'start') message.value = '微信启动请求已提交。';
    if (action === 'stop') message.value = '等待当前长轮询退出，不影响会话记录。';
  } catch (cause) { if (client === session.client) error.value = String(cause); }
  finally { busy.value = false; }
}
watch([() => session.client, available], () => { state.value = null; void request('get', true); }, { immediate: true });
const timer = setInterval(() => {
  if (state.value?.qrContent && (state.value.loginState === 0 || state.value.loginState === 2)) void request('login.poll');
  else if (state.value?.running || state.value?.requested) void request('get');
}, 2000);
onBeforeUnmount(() => clearInterval(timer));
</script>

<template>
  <section class="weixin-panel">
    <h2>微信消息渠道</h2>
    <p>通过 iLink 接收微信文字消息，使用设备的模型与主人资料，回复回到原会话。启动代表允许此渠道发起对话；写入操作仍在 Nyabot 任务页确认。</p>
    <p v-if="!supported">当前固件未编译微信通道。</p>
    <p v-if="error" role="alert" class="error">{{ error }}</p>
    <p role="status">{{ status }}<span v-if="state?.lastError"> · 错误 {{ state.lastError }}</span></p>
    <div class="actions">
      <MdButton :disabled="!available || !stopped || busy || dirty" @click="request('login')">获取微信登录二维码</MdButton>
      <MdButton variant="tonal" :disabled="!available || !stopped || busy || dirty || !state?.tokenSet" @click="request('start')">启动微信通道</MdButton>
      <MdButton variant="tonal" :disabled="!available || stopped || busy" @click="request('stop')">停止微信通道</MdButton>
      <MdButton variant="text" :disabled="!available || busy" @click="request('get', true)">重新读取微信配置</MdButton>
    </div>
    <div v-if="qrImage" class="qr"><img :src="qrImage" alt="微信登录二维码" width="256" height="256" /><p>{{ loginStatus }}</p></div>
    <p v-else-if="state?.loginState === 1 || state?.loginState === -3" role="status">{{ loginStatus }}</p>
    <p>{{ message }}</p>
    <details><summary>高级配置与已有凭据</summary>
      <p>通常使用默认服务地址并扫码登录。已有凭据可在下方导入；不会回读已保存 Token。修改前请停止通道。</p>
      <form @submit.prevent="request('save')">
        <MdTextField v-model="form.host" label="微信服务主机" :disabled="!available || !stopped || busy" />
        <MdTextField v-model="form.port" label="微信服务端口" :disabled="!available || !stopped || busy" />
        <MdTextField v-model="form.token" label="微信 Bot Token" type="password" autocomplete="off" :placeholder="state?.tokenSet ? '已设置，留空保留' : '可通过扫码获得'" :disabled="!available || !stopped || busy || clearToken" />
        <label><input v-model="clearToken" type="checkbox" aria-label="清除微信凭据" :disabled="!available || !stopped || busy" />清除已保存微信凭据</label>
        <MdButton :disabled="!available || !stopped || busy || !dirty">保存微信配置</MdButton>
        <p v-if="dirty">微信配置尚未保存</p>
      </form>
    </details>
  </section>
</template>

<style scoped>
.weixin-panel { max-width: 760px; width: 100%; margin: 0 auto; line-height: 1.6; }
.actions { display: flex; flex-wrap: wrap; gap: 8px; }
.qr { display: flex; flex-direction: column; align-items: center; margin: 24px 0; }
.qr img { max-width: 100%; height: auto; border-radius: 12px; }
details { margin-top: 24px; }
summary { cursor: pointer; min-height: 44px; }
form { display: flex; flex-direction: column; gap: 16px; }
label:has(input[type="checkbox"]) { display: flex; gap: 8px; align-items: center; min-height: 44px; }
.error { color: var(--md-error); }
</style>
