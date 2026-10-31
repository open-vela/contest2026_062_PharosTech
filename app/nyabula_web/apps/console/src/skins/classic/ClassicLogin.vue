<script setup lang="ts">
/* Login / register (cloud.md §4 auth endpoints). */
import { MdButton, MdCard, ThemeSwitcher } from '@nyabula/ui';
import { useLogin } from '../../logic/useLogin';

const f = useLogin();
</script>

<template>
  <div class="login-wrap">
    <h1 class="brand">Nyabula</h1>
    <p class="sub">Cloud 控制中心</p>
    <MdCard class="card">
      <div class="tabs">
        <button class="tab" :class="{ active: f.mode.value === 'login' }" @click="f.setMode('login')">登录</button>
        <button class="tab" :class="{ active: f.mode.value === 'register' }" @click="f.setMode('register')">注册</button>
      </div>

      <label class="lbl">邮箱</label>
      <input v-model="f.email.value" type="text" spellcheck="false" placeholder="you@example.com" autocomplete="username" @keyup.enter="f.submit" />

      <template v-if="f.mode.value === 'register'">
        <label class="lbl top-gap">昵称</label>
        <input v-model="f.name.value" type="text" placeholder="猫猫的主人" @keyup.enter="f.submit" />
      </template>

      <label class="lbl top-gap">密码</label>
      <input v-model="f.password.value" type="password" class="pwd" placeholder="••••••••" autocomplete="current-password" @keyup.enter="f.submit" />

      <div class="row">
        <MdButton :disabled="f.auth.busy" @click="f.submit">
          {{ f.auth.busy ? '请稍候…' : f.mode.value === 'login' ? '登录' : '注册' }}
        </MdButton>
      </div>
      <p v-if="f.localError.value || f.auth.lastError" class="err">{{ f.localError.value ?? f.auth.lastError }}</p>
    </MdCard>
    <div class="theme"><ThemeSwitcher /></div>
  </div>
</template>

<style scoped>
.login-wrap { max-width: 420px; margin: 10vh auto 0; padding: 0 20px 40px; text-align: center; }
.brand { font-size: 44px; color: var(--md-primary); letter-spacing: 2px; }
.sub { color: var(--md-on-surface-variant); margin: 6px 0 26px; }
.card { text-align: left; }
.tabs { display: flex; gap: 8px; margin-bottom: 18px; }
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
.tab.active { background: var(--md-secondary-container); color: var(--md-on-secondary-container); border-color: transparent; }
.lbl { display: block; font-size: 12.5px; color: var(--md-on-surface-variant); margin-bottom: 8px; letter-spacing: 1px; }
.top-gap { margin-top: 14px; }
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
.pwd:focus { border-color: var(--md-primary); }
.row { margin-top: 18px; display: flex; justify-content: flex-end; }
.err { color: var(--md-error); font-size: 13px; margin: 14px 0 0; }
.theme { display: flex; justify-content: center; margin-top: 22px; }
</style>
