<script setup lang="ts">
/* Nebula sign-in: the eyes watch the form (curious while typing, close
 * politely on the password, sad on a failed attempt, thinking while the
 * request is in flight). Real POST /auth/login | /auth/register. */
import { computed, ref, watch } from 'vue';
import { ThemeSwitcher, UiIcon } from '@nyabula/ui';
import LiveEyes from '../../components/LiveEyes.vue';
import { useLogin } from '../../logic/useLogin';

const f = useLogin();
const focus = ref<'email' | 'pw' | 'name' | null>(null);
const shake = ref(false);
const err = computed(() => f.localError.value ?? f.auth.lastError);

const eyeMode = computed(() =>
  f.auth.busy ? 'processing' : focus.value === 'pw' ? 'sleepy' : err.value ? 'sad' : focus.value ? 'curious' : 'idle',
);

async function submit() {
  const ok = await f.submit();
  if (!ok) {
    shake.value = true;
    window.setTimeout(() => (shake.value = false), 460);
  }
}
watch(f.mode, () => (f.auth.lastError = null));
</script>

<template>
  <div class="login">
    <div class="theme"><ThemeSwitcher /></div>
    <div class="visor"><LiveEyes :mode="eyeMode" /></div>
    <form class="card nb-glass" :class="{ shake }" @submit.prevent="submit">
      <p class="nb-kicker">Nyabula Cloud</p>
      <h1 class="nb-title">{{ f.mode.value === 'login' ? '欢迎回来' : '创建账号' }}</h1>
      <p class="sub">在任何地方，看看你的猫在做什么。</p>

      <div class="nb-seg seg">
        <span class="nb-seg-light" :style="{ width: 'calc(50% - 4px)', transform: `translateX(${f.mode.value === 'login' ? 0 : 100}%)` }" />
        <button type="button" :class="{ on: f.mode.value === 'login' }" @click="f.setMode('login')">登录</button>
        <button type="button" :class="{ on: f.mode.value === 'register' }" @click="f.setMode('register')">注册</button>
      </div>

      <Transition name="grow">
        <label v-if="f.mode.value === 'register'" class="field">
          <UiIcon name="person" :size="18" />
          <input v-model="f.name.value" class="nb-input" placeholder="昵称" autocomplete="nickname" @focus="focus = 'name'" @blur="focus = null" />
        </label>
      </Transition>
      <label class="field">
        <UiIcon name="article" :size="18" />
        <input v-model="f.email.value" class="nb-input" type="email" placeholder="邮箱" autocomplete="username" spellcheck="false" @focus="focus = 'email'" @blur="focus = null" />
      </label>
      <label class="field">
        <UiIcon name="lock" :size="18" />
        <input
          v-model="f.password.value"
          class="nb-input"
          type="password"
          placeholder="密码"
          :autocomplete="f.mode.value === 'login' ? 'current-password' : 'new-password'"
          @focus="focus = 'pw'"
          @blur="focus = null"
        />
      </label>
      <Transition name="grow"><p v-if="err" class="nb-err"><UiIcon name="error" :size="15" /> {{ err }}</p></Transition>

      <button class="nb-btn primary go" type="submit" :disabled="f.auth.busy">
        <span v-if="!f.auth.busy">{{ f.mode.value === 'login' ? '登录' : '注册并登录' }}</span>
        <span v-else class="spin" />
        <UiIcon v-if="!f.auth.busy" name="arrow_forward" :size="18" />
      </button>
      <p class="foot">Cloud 只统计帧数与在线时长，不记录帧内容</p>
    </form>
  </div>
</template>

<style scoped>
.login { position: relative; min-height: 100dvh; display: flex; flex-direction: column; align-items: center; justify-content: center; padding: 24px 16px 60px; gap: 14px; }
.theme { position: absolute; top: 16px; right: 16px; }
.visor {
  width: min(360px, 84vw);
  height: 150px;
  border-radius: 999px;
  background: #000;
  border: 1px solid var(--line);
  box-shadow: 0 0 0 6px var(--wash), 0 30px 80px -30px rgba(var(--acc-rgb), 0.6);
  overflow: hidden;
  animation: in 1.2s var(--ease) both;
}
@keyframes in { from { opacity: 0; transform: translateY(-20px) scale(0.9); filter: blur(10px); } }
.card { width: min(420px, 100%); padding: 30px 30px 24px; display: flex; flex-direction: column; gap: 12px; animation: nbc-rise 0.9s var(--ease) 0.15s both; }
.card.shake { animation: shake 0.45s var(--ease); }
@keyframes shake { 20% { transform: translateX(-8px); } 40% { transform: translateX(7px); } 60% { transform: translateX(-4px); } 80% { transform: translateX(2px); } }
.card h1 { margin: 2px 0 0; font-size: 32px; }
.sub { color: var(--sub); margin: 0 0 8px; }
.seg { margin-bottom: 4px; }
.field { position: relative; display: block; color: var(--dim); }
.field :deep(.ui-icon) { position: absolute; left: 14px; top: 50%; transform: translateY(-50%); pointer-events: none; transition: color 0.2s; }
.field:focus-within :deep(.ui-icon) { color: var(--acc); }
.field .nb-input { padding-left: 42px; }
.go { justify-content: center; padding: 13px; margin-top: 6px; font-size: 15px; }
.go:disabled { opacity: 0.85; }
.spin { width: 18px; height: 18px; border-radius: 50%; border: 2px solid color-mix(in srgb, var(--md-on-primary) 30%, transparent); border-top-color: var(--md-on-primary); animation: sp 0.7s linear infinite; }
@keyframes sp { to { transform: rotate(360deg); } }
.foot { text-align: center; color: var(--dim); font-size: 11.5px; margin: 4px 0 0; }
.grow-enter-active, .grow-leave-active { transition: opacity 0.3s, transform 0.4s var(--ease); }
.grow-enter-from, .grow-leave-to { opacity: 0; transform: translateY(-6px); }
@media (max-width: 480px) { .card { padding: 24px 20px 20px; } .visor { height: 120px; } }
</style>
