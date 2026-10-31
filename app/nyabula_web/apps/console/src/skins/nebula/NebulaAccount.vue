<script setup lang="ts">
/* Nebula account page: signed-in user, appearance (skin / mode / palette,
 * shared with the device WebUI), sign out. */
import { UiIcon } from '@nyabula/ui';
import AppearancePanel from '../../components/AppearancePanel.vue';
import { useAuthStore } from '../../stores/auth';
import { useNavStore } from '../../stores/nav';

const auth = useAuthStore();
const nav = useNavStore();
async function logout() {
  if (await auth.logout()) nav.go('overview');
}
</script>

<template>
  <div class="acc">
    <header class="hd nb-in">
      <p class="nb-kicker">账号</p>
      <h1 class="nb-title">{{ auth.user?.name || '我的账号' }}</h1>
    </header>
    <p v-if="auth.lastError" role="alert">{{ auth.lastError }}</p>
    <div class="grid">
      <article class="panel nb-glass nb-in me" style="--d: 80ms">
        <span class="av">{{ (auth.user?.name || auth.user?.email || '?').slice(0, 1).toUpperCase() }}</span>
        <dl>
          <div><dt>昵称</dt><dd>{{ auth.user?.name || '—' }}</dd></div>
          <div><dt>邮箱</dt><dd>{{ auth.user?.email }}</dd></div>
          <div v-if="auth.user?.id"><dt>用户 ID</dt><dd class="nb-mono">{{ auth.user.id }}</dd></div>
        </dl>
        <button class="nb-btn danger" @click="logout"><UiIcon name="logout" :size="16" /> 退出登录</button>
      </article>
      <article class="panel nb-glass nb-in" style="--d: 160ms">
        <h3>外观</h3>
        <p class="p-sub">仅保存于当前站点的浏览器中。</p>
        <AppearancePanel />
      </article>
    </div>
  </div>
</template>

<style scoped>
.acc { display: flex; flex-direction: column; gap: 18px; }
.hd h1 { font-size: clamp(28px, 3.4vw, 44px); margin: 6px 0 0; }
.grid { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1.6fr); gap: 18px; align-items: start; }
.panel { padding: 22px; }
.panel h3 { margin: 0; font: 600 16px var(--font-body); }
.p-sub { margin: 2px 0 18px; color: var(--sub); font-size: 12px; }
.me { display: flex; flex-direction: column; align-items: flex-start; gap: 16px; }
.av { width: 64px; height: 64px; border-radius: 22px; display: grid; place-items: center; background: linear-gradient(135deg, var(--acc), var(--acc2)); color: var(--md-on-primary); font: 400 28px var(--font-title); box-shadow: 0 16px 40px -16px rgba(var(--acc-rgb), 0.8); }
dl { margin: 0; display: grid; gap: 10px; width: 100%; }
dl div { display: flex; justify-content: space-between; gap: 12px; }
dt { color: var(--sub); font-size: 13px; }
dd { margin: 0; font-weight: 600; text-align: right; word-break: break-all; }
@media (max-width: 900px) { .grid { grid-template-columns: minmax(0, 1fr); } }
</style>
