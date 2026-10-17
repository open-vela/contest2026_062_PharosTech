<script setup lang="ts">
/* Phone: known devices first (tap to connect), "add" opens a sheet form. */
import { onMounted, ref } from 'vue';
import { BottomSheet, MdButton, UiIcon, NyabulaLogo } from '@nyabula/ui';
import { useConnectPage } from './connect.logic';
import KnownDeviceList from './KnownDeviceList.vue';
import ConnectForm from './ConnectForm.vue';

const page = useConnectPage();
const sheet = ref(false);
onMounted(() => {
  if (page.account.loggedIn) void page.account.refreshDevices().catch(() => undefined);
  if (!page.known.value.length || page.reauth) sheet.value = true;
});
</script>

<template>
  <div class="connect-phone">
    <header class="hero">
      <div class="hero-mark"><NyabulaLogo :size="60" /></div>
      <h2 class="page-title">连接 Nyabula</h2>
      <p class="page-sub">选择最近的设备，或添加新设备</p>
    </header>
    <KnownDeviceList :page="page" />
    <div class="fab-row">
      <MdButton @click="sheet = true"><UiIcon name="add" :size="18" /> 添加设备</MdButton>
    </div>
    <BottomSheet :open="sheet" :title="page.reauth ? '重新认证' : '添加设备'" max-height="92vh" @close="sheet = false">
      <ConnectForm :page="page" />
    </BottomSheet>
  </div>
</template>

<style scoped>
.connect-phone { padding: 12px 16px 100px; display: flex; flex-direction: column; gap: 14px; }
.hero { text-align: center; display: flex; flex-direction: column; align-items: center; gap: 6px; padding: 12px 0 6px; }
.hero-mark {
  width: 60px;
  height: 60px;
  border-radius: 18px;
  display: grid;
  place-items: center;
  margin-bottom: 6px;
  border-radius: 13px;
  overflow: hidden;
}
.fab-row { position: fixed; left: 0; right: 0; bottom: calc(var(--shell-bottom) + 16px + var(--safe-b)); display: flex; justify-content: center; pointer-events: none; }
.fab-row :deep(.md-btn) { pointer-events: auto; display: inline-flex; align-items: center; gap: 6px; box-shadow: var(--md-elev-2); padding: 12px 22px; }
</style>
