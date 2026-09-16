<script setup lang="ts">
/* Login / register (cloud.md §4 auth endpoints). */
import { ref } from 'vue';
import { MdButton, MdCard } from '@nyabula/ui';
import { useAuthStore } from '../stores/auth';

const auth = useAuthStore();
const mode = ref<'login' | 'register'>('login');
const email = ref('');
const password = ref('');
const name = ref('');

async function submit() {
  if (!email.value || !password.value) return;
  if (mode.value === 'login') {
    await auth.login(email.value, password.value);
  } else {
    if (!name.value) return;
    await auth.register(email.value, password.value, name.value);
  }
}
</script>

<template>
  <div class="login-wrap">
    <h1 class="brand">Nyabula</h1>
    <p class="sub">Cloud 控制中心</p>
    <MdCard class="card">
      <div class="tabs">
        <button class="tab" :class="{ active: mode === 'login' }" @click="mode = 'login'">登录</button>
        <button class="tab" :class="{ active: mode === 'register' }" @click="mode = 'register'">注册</button>
      </div>

      <label class="lbl">邮箱</label>
      <input v-model="email" type="text" spellcheck="false" placeholder="you@example.com" @keyup.enter="submit" />

      <template v-if="mode === 'register'">
        <label class="lbl top-gap">昵称</label>
        <input v-model="name" type="text" placeholder="猫猫的主人" @keyup.enter="submit" />
      </template>

      <label class="lbl top-gap">密码</label>
      <input v-model="password" type="password" class="pwd" placeholder="••••••••" @keyup.enter="submit" />

      <div class="row">
        <MdButton :disabled="auth.busy" @click="submit">
          {{ auth.busy ? '请稍候…' : mode === 'login' ? '登录' : '注册' }}
        </MdButton>
      </div>
      <p v-if="auth.lastError" class="err">{{ auth.lastError }}</p>
    </MdCard>
  </div>
</template>

<style scoped>
.login-wrap {
  max-width: 420px;
  margin: 10vh auto 0;
  padding: 0 20px;
  text-align: center;
}
.brand {
  font-size: 44px;
  color: var(--md-primary);
  letter-spacing: 2px;
}
.sub {
  color: var(--md-on-surface-variant);
  margin: 6px 0 26px;
}
.card {
  text-align: left;
}
.tabs {
  display: flex;
  gap: 8px;
  margin-bottom: 18px;
}
.tab {
  flex: 1;
  border: 1px solid var(--md-outline-variant);
  background: transparent;
  color: var(--md-on-surface-variant);
  border-radius: var(--radius-s);
  padding: 8px 0;
  font: 600 13.5px var(--font-body);
  cursor: pointer;
}
.tab.active {
  background: var(--md-secondary-container);
  color: var(--md-on-secondary-container);
  border-color: transparent;
}
.lbl {
  display: block;
  font-size: 12.5px;
  color: var(--md-on-surface-variant);
  margin-bottom: 8px;
  letter-spacing: 1px;
}
.top-gap {
  margin-top: 14px;
}
.pwd {
  background: var(--md-surface-container-high);
  color: var(--md-on-surface);
  border: 1px solid var(--md-outline-variant);
  border-radius: var(--radius-m);
  padding: 12px 16px;
  font: 600 15px var(--font-body);
  outline: none;
  width: 100%;
}
.pwd:focus {
  border-color: var(--md-primary);
}
.row {
  margin-top: 18px;
  display: flex;
  justify-content: flex-end;
}
.err {
  color: var(--md-error);
  font-size: 13px;
  margin: 14px 0 0;
}
</style>
