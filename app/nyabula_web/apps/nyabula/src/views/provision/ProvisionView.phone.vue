<script setup lang="ts">
/* Phone (portrait, opened from the QR code): one full-width column.
 * Device build: bare page in the phone gate frame (no app shell). */
import GateFrame from '../../components/gate/GateFrame.phone.vue';
import { useProvisionPage } from './provision.logic';
import ProvisionBody from './ProvisionBody.vue';

const page = useProvisionPage();
/* Compile-time constant: the unused frame is dropped from each build. */
const deviceBuild = __NYA_DEVICE__;
</script>

<template>
  <GateFrame v-if="deviceBuild" title="给设备配置 WiFi" sub="选择家里的 WiFi 并输入密码，设备会自己连上去。">
    <ProvisionBody :page="page" />
  </GateFrame>
  <div v-else class="provision-phone">
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
