<script setup lang="ts">
/* Instrument account page: session readout (GET /auth/me), appearance
 * (shared with the device WebUI) and sign out. */
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
  <div class="acc ins-boot">
    <p v-if="auth.lastError" role="alert">{{ auth.lastError }}</p>
    <section class="ins-panel">
      <div class="ins-panel-head"><span class="ins-label"><span class="ins-idx">A</span> SESSION · 当前账号</span><span class="ins-label"><span class="ins-led on" /> ACTIVE</span></div>
      <dl class="kv">
        <div><dt class="ins-label">NAME</dt><dd>{{ auth.user?.name || '--' }}</dd></div>
        <div><dt class="ins-label">EMAIL</dt><dd class="mono">{{ auth.user?.email }}</dd></div>
        <div v-if="auth.user?.id"><dt class="ins-label">USER ID</dt><dd class="mono">{{ auth.user.id }}</dd></div>
      </dl>
      <button class="ins-key danger" @click="logout"><UiIcon name="logout" :size="15" /> 退出登录 LOGOUT</button>
    </section>

    <section class="ins-panel">
      <div class="ins-panel-head"><span class="ins-label"><span class="ins-idx">B</span> APPEARANCE · 外观（与设备 WebUI 同步）</span></div>
      <AppearancePanel flat />
    </section>
  </div>
</template>

<style scoped>
.acc { padding: 16px 20px 28px; max-width: 1100px; display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1.6fr); gap: 14px; align-items: start; }
.kv { margin: 0 0 16px; display: flex; flex-direction: column; }
.kv div { display: flex; justify-content: space-between; align-items: baseline; gap: 12px; padding: 9px 0; border-top: 1px solid var(--c-line); }
.kv div:first-child { border-top: none; }
dd { margin: 0; font-weight: 600; word-break: break-all; text-align: right; }
.mono { font: 600 12.5px var(--c-mono); }
@media (max-width: 860px) { .acc { grid-template-columns: minmax(0, 1fr); padding: 12px; } }
</style>
