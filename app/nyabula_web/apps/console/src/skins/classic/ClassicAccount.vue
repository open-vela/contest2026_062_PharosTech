<script setup lang="ts">
/* Account: signed-in user (GET /auth/me), appearance, sign out. */
import { MdButton, MdCard, UiIcon } from '@nyabula/ui';
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
  <div class="page">
    <h2 class="title">账号</h2>
    <p v-if="auth.lastError" role="alert">{{ auth.lastError }}</p>
    <div class="grid">
      <MdCard title="当前账号">
        <dl class="kv">
          <div><dt>昵称</dt><dd>{{ auth.user?.name || '—' }}</dd></div>
          <div><dt>邮箱</dt><dd>{{ auth.user?.email }}</dd></div>
          <div v-if="auth.user?.id"><dt>用户 ID</dt><dd class="mono">{{ auth.user.id }}</dd></div>
        </dl>
        <MdButton variant="outlined" class="out" @click="logout"><UiIcon name="logout" :size="16" /> 登出</MdButton>
      </MdCard>
      <MdCard title="外观">
        <AppearancePanel />
      </MdCard>
    </div>
  </div>
</template>

<style scoped>
.page { padding: 22px; max-width: 960px; margin: 0 auto; }
.title { font-size: 22px; margin-bottom: 18px; }
.grid { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1.4fr); gap: 16px; align-items: start; }
.kv { display: flex; flex-direction: column; gap: 10px; margin: 0 0 16px; }
.kv > div { display: flex; justify-content: space-between; gap: 16px; }
.kv dt { color: var(--md-on-surface-variant); font-size: 13px; }
.kv dd { margin: 0; font-weight: 600; word-break: break-all; text-align: right; }
.mono { font-family: ui-monospace, monospace; }
.out :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
@media (max-width: 800px) { .grid { grid-template-columns: 1fr; } }
</style>
