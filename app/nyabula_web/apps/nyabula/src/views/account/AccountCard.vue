<script setup lang="ts">
/* Signed-in identity card (desktop context panel / inline elsewhere). */
import { MdButton, MdCard, UiIcon } from '@nyabula/ui';
import type { useAccountPage } from './account.logic';

const props = defineProps<{ page: ReturnType<typeof useAccountPage> }>();
const p = props.page;
</script>

<template>
  <MdCard title="账号">
    <div class="who">
      <div class="avatar"><UiIcon name="person" :size="26" /></div>
      <div class="who-body">
        <div class="who-name">{{ p.account.user?.name || '未命名' }}</div>
        <div class="muted who-mail">{{ p.account.user?.email }}</div>
      </div>
    </div>
    <dl class="kv" style="margin-top: 14px">
      <div><dt>设备</dt><dd>{{ p.account.overview?.devices ?? p.account.devices.length }}</dd></div>
      <div><dt>在线</dt><dd>{{ p.account.overview?.online ?? p.account.devices.filter((d) => d.online).length }}</dd></div>
    </dl>
    <div class="row wrap" style="margin-top: 14px; gap: 8px">
      <MdButton variant="outlined" @click="p.reload()"><UiIcon name="refresh" :size="16" /> 刷新</MdButton>
      <MdButton variant="text" @click="p.logout()"><UiIcon name="logout" :size="16" /> 退出登录</MdButton>
    </div>
  </MdCard>
</template>

<style scoped>
.who { display: flex; align-items: center; gap: 12px; }
.avatar {
  width: 48px;
  height: 48px;
  border-radius: 50%;
  display: grid;
  place-items: center;
  background: var(--md-primary-container);
  color: var(--md-on-primary-container);
  flex: none;
}
.who-name { font-weight: 600; font-size: 15px; color: var(--md-on-surface); }
.who-mail { font-size: 12.5px; word-break: break-all; }
</style>
