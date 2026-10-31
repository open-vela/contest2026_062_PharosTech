<script setup lang="ts">
/* Terminal login: dot-matrix wordmark, mono fields and a line-by-line log
 * of the real requests (auth, then the fleet sync) before the console
 * opens. Every log line reflects an actual call and its actual result. */
import { onBeforeUnmount, ref } from 'vue';
import { ThemeSwitcher } from '@nyabula/ui';
import DotMatrix from '../../components/DotMatrix.vue';
import { useLogin } from '../../logic/useLogin';
import { useDevicesStore } from '../../stores/devices';
import { handshaking } from './handshake';

const f = useLogin();
const devices = useDevicesStore();
const log = ref<{ t: string; s: 'run' | 'ok' | 'err'; r?: string }[]>([]);
const timers: number[] = [];
const wait = (ms: number) => new Promise<void>((r) => timers.push(window.setTimeout(r, ms)));
onBeforeUnmount(() => {
  timers.forEach(clearTimeout);
  handshaking.value = false;
});

function line(t: string) {
  log.value.push({ t, s: 'run' });
  return log.value[log.value.length - 1]!;
}

async function submit() {
  if (f.auth.busy || handshaking.value) return;
  log.value = [];
  handshaking.value = true;
  const auth = line(`POST  /auth/${f.mode.value}`);
  const ok = await f.submit();
  if (!ok) {
    auth.s = 'err';
    auth.r = f.localError.value ?? f.auth.lastError ?? 'FAILED';
    handshaking.value = false;
    return;
  }
  auth.s = 'ok';
  auth.r = f.auth.user?.email;
  await wait(180);
  const sync = line('GET   /devices');
  await devices.refresh();
  sync.s = devices.lastError ? 'err' : 'ok';
  sync.r = devices.lastError ?? `${devices.devices.length} DEVICES · ${devices.devices.filter((d) => d.online).length} ONLINE`;
  await wait(420);
  handshaking.value = false;
}
</script>

<template>
  <div class="login">
    <div class="scan" aria-hidden="true" />
    <div class="theme"><ThemeSwitcher /></div>
    <div class="box">
      <div class="mark">
        <DotMatrix text="NYABULA" :dot="7" :gap="2.4" />
        <DotMatrix text="CLOUD" :dot="7" :gap="2.4" color="var(--c-sig)" />
      </div>
      <p class="ins-label sub">DEVICE RELAY · FLEET CONSOLE</p>

      <form class="ins-panel form" @submit.prevent="submit">
        <div class="ins-panel-head">
          <div class="ins-seg">
            <button type="button" :class="{ on: f.mode.value === 'login' }" @click="f.setMode('login')">LOGIN</button>
            <button type="button" :class="{ on: f.mode.value === 'register' }" @click="f.setMode('register')">REGISTER</button>
          </div>
          <span class="ins-label"><span class="ins-led on blink" /> SESSION</span>
        </div>
        <label v-if="f.mode.value === 'register'" class="fld"><span class="ins-label">NAME · 昵称</span><input v-model="f.name.value" class="ins-input" autocomplete="nickname" :disabled="handshaking" /></label>
        <label class="fld"><span class="ins-label">EMAIL</span><input v-model="f.email.value" class="ins-input" type="email" autocomplete="username" spellcheck="false" :disabled="handshaking" /></label>
        <label class="fld">
          <span class="ins-label">PASSWORD</span>
          <input v-model="f.password.value" class="ins-input" type="password" :autocomplete="f.mode.value === 'login' ? 'current-password' : 'new-password'" placeholder="••••••••" :disabled="handshaking" />
        </label>
        <button class="ins-key solid go" :disabled="handshaking">{{ handshaking ? 'CONNECTING…' : f.mode.value === 'login' ? '登录 ENTER ↵' : '注册 CREATE ↵' }}</button>

        <TransitionGroup v-if="log.length" name="ln" tag="ol" class="log">
          <li v-for="(l, i) in log" :key="i">
            <span class="ins-dim">&gt;</span>
            <span class="lt">{{ l.t }}</span>
            <span class="dots" />
            <span :class="l.s === 'ok' ? 'ins-sig' : l.s === 'err' ? 'ins-warn' : 'ins-dim run'">{{ l.s === 'ok' ? 'OK' : l.s === 'err' ? 'ERR' : '···' }}</span>
            <span v-if="l.r" class="lr" :class="{ 'ins-warn': l.s === 'err' }">{{ l.r }}</span>
          </li>
        </TransitionGroup>
      </form>
      <p class="ins-label foot">CLOUD 只统计帧数与在线时长 · 不记录帧内容</p>
    </div>
  </div>
</template>

<style scoped>
.login { position: relative; min-height: 100dvh; display: grid; place-items: center; padding: 24px 16px; overflow: hidden; }
.scan { position: absolute; inset: 0; pointer-events: none; background: repeating-linear-gradient(0deg, var(--c-grid) 0 1px, transparent 1px 3px); }
.scan::after { content: ''; position: absolute; left: 0; right: 0; top: 0; height: 120px; background: linear-gradient(transparent, rgba(var(--c-sig-rgb), 0.06), transparent); animation: sweep 5s linear infinite; }
@keyframes sweep { from { transform: translateY(-120px); } to { transform: translateY(100vh); } }
.theme { position: absolute; top: 16px; right: 16px; z-index: 2; }
.box { position: relative; width: 100%; max-width: 440px; display: flex; flex-direction: column; gap: 16px; }
.mark { display: flex; flex-direction: column; gap: 10px; color: var(--c-fg); }
.mark :deep(.dm) { max-width: 100%; height: auto; }
.sub { margin: 0 0 8px; }
.form { display: flex; flex-direction: column; gap: 14px; padding: 18px; }
.fld { display: flex; flex-direction: column; gap: 7px; }
.go { min-height: 46px; font-size: 12px; }
.log { list-style: none; margin: 0; padding: 12px 0 0; border-top: 1px dashed var(--c-line); font: 500 12px/1.9 var(--c-mono); }
.log li { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; }
.lt { white-space: pre; }
.dots { flex: 1; min-width: 20px; height: 1px; margin: 0 6px; background-image: linear-gradient(90deg, var(--c-line-2) 50%, transparent 50%); background-size: 4px 1px; }
.lr { flex-basis: 100%; padding-left: 14px; color: var(--c-dim); font-size: 11px; line-height: 1.5; word-break: break-all; }
.run { animation: insc-blink 0.5s steps(2, jump-none) infinite; }
.ln-enter-active { transition: opacity 160ms, transform 160ms var(--c-ease); }
.ln-enter-from { opacity: 0; transform: translateX(-6px); }
.foot { text-align: center; margin: 8px 0 0; font-size: 9px; }
</style>
