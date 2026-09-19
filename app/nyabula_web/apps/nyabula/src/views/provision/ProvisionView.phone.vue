<script setup lang="ts">
/* Phone (portrait, opened from the QR code): one full-width column. */
import { useProvisionPage } from './provision.logic';
import ProvisionBody from './ProvisionBody.vue';

const page = useProvisionPage();
</script>

<template>
  <div class="provision-phone">
    <header class="head">
      <h2 class="page-title">给设备配置 WiFi</h2>
      <p class="page-sub">选择家里的 WiFi 并输入密码，设备会自己连上去。</p>
    </header>
    <ProvisionBody :page="page" />
  </div>
</template>

<style scoped>
.provision-phone { padding: 12px 16px calc(28px + var(--safe-b)); display: flex; flex-direction: column; gap: 6px; }
.head .page-sub { margin-bottom: 10px; }
</style>
