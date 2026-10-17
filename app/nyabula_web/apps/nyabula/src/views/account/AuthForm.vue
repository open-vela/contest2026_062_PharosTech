<script setup lang="ts">
/* Login / register switcher used by every account variant when signed out. */
import { MdButton, MdCard, MdTextField, SegmentedTabs } from '@nyabula/ui';
import type { useAccountPage } from './account.logic';

const props = defineProps<{ page: ReturnType<typeof useAccountPage> }>();
const p = props.page;
</script>

<template>
  <MdCard>
    <SegmentedTabs v-model="p.authMode.value" :items="p.authTabs" stretch />
    <form class="stack auth-form" @submit.prevent="p.submitAuth()">
      <MdTextField v-if="p.authMode.value === 'register'" v-model="p.name.value" label="昵称" icon="person" placeholder="怎么称呼你" autocomplete="nickname" />
      <MdTextField v-model="p.email.value" label="邮箱" type="email" icon="cloud" inputmode="email" placeholder="you@example.com" autocomplete="email" />
      <MdTextField
        v-model="p.password.value"
        label="密码"
        type="password"
        icon="lock"
        placeholder="至少 6 位"
        :autocomplete="p.authMode.value === 'register' ? 'new-password' : 'current-password'"
        :error="p.authError.value"
        @enter="p.submitAuth()"
      />
      <MdButton :disabled="!p.authValid.value || p.authBusy.value">
        {{ p.authBusy.value ? '请稍候…' : p.authMode.value === 'login' ? '登录' : '注册并登录' }}
      </MdButton>
      <p class="muted hint">Cloud 账号用于远程连接设备、查看统计与专属功能。</p>
    </form>
  </MdCard>
</template>

<style scoped>
.auth-form { margin-top: 16px; }
.hint { font-size: 12.5px; margin: 0; text-align: center; }
</style>
