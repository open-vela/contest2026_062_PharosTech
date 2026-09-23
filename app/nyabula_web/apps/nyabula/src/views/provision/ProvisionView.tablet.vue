<script setup lang="ts">
/* Tablet. Device build: bare page in the tablet gate frame (brand panel
 * beside the form in landscape, one touch-sized column in portrait).
 * Hosted build: unchanged, the narrow column the desktop variant shows. */
import GateFrame from '../../components/gate/GateFrame.tablet.vue';
import { useProvisionPage } from './provision.logic';
import ProvisionBody from './ProvisionBody.vue';

const page = useProvisionPage();
/* Compile-time constant: the unused frame is dropped from each build. */
const deviceBuild = __NYA_DEVICE__;
</script>

<template>
  <GateFrame v-if="deviceBuild" wide title="给设备配置 WiFi" sub="选择要加入的 WiFi 并输入密码，设备会保存并自动连接。">
    <ProvisionBody :page="page" />
  </GateFrame>
  <div v-else class="page narrow provision-desktop">
    <h2 class="page-title">给设备配置 WiFi</h2>
    <p class="page-sub">选择要加入的 WiFi 并输入密码，设备会保存并自动连接。</p>
    <ProvisionBody :page="page" />
  </div>
</template>

<style scoped>
.provision-desktop { max-width: 560px; }
</style>
