<script setup lang="ts">
/* Desktop / tablet: the same flow in a narrow centred column. */
import { useProvisionPage } from './provision.logic';
import ProvisionBody from './ProvisionBody.vue';

const page = useProvisionPage();
</script>

<template>
  <div class="page narrow provision-desktop">
    <h2 class="page-title">给设备配置 WiFi</h2>
    <p class="page-sub">选择要加入的 WiFi 并输入密码，设备会保存并自动连接。</p>
    <ProvisionBody :page="page" />
  </div>
</template>

<style scoped>
.provision-desktop { max-width: 560px; }
</style>
