<script setup lang="ts">
import { ref } from 'vue';
import { MdButton, MdTextField } from '@nyabula/ui';
import { useRemoteStore } from '../stores/remote';
const remote = useRemoteStore();
const password = ref('');
const busy = ref(false);
async function submit() {
  if (busy.value || !password.value) return;
  busy.value = true;
  if (await remote.login(password.value)) password.value = '';
  busy.value = false;
}
</script>
<template>
  <form v-if="remote.passwordRequired" class="device-login" @submit.prevent="submit">
    <h3>登录设备</h3>
    <p v-if="!remote.passwordSet">请先在设备的局域网面板设置访问密码，再使用 Cloud 远程连接。</p>
    <template v-else>
      <p>输入设备访问密码，不是 Cloud 账号密码。</p>
      <MdTextField v-model="password" type="password" label="设备密码" autocomplete="current-password" :disabled="busy" />
      <p v-if="remote.lastError" role="alert">{{ remote.lastError }}</p>
      <MdButton type="submit" :disabled="busy || !password">{{ busy ? '登录中…' : '连接设备' }}</MdButton>
    </template>
  </form>
</template>
<style scoped>
.device-login { position: absolute; inset: 12px; z-index: 6; margin: auto; width: min(360px, calc(100% - 24px)); height: fit-content; max-height: calc(100% - 24px); overflow: auto; display: grid; gap: 12px; padding: 20px; box-sizing: border-box; border-radius: var(--radius-l, 16px); background: var(--md-surface-container-high); color: var(--md-on-surface); box-shadow: 0 8px 30px #0004; }
h3, p { margin: 0; }
p { font-size: 14px; line-height: 1.5; }
[role="alert"] { color: var(--md-error); }
</style>
