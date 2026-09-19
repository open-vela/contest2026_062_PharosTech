<script setup lang="ts">
/* Desktop: centred fixed-width column (a computer cannot scan the QR code, so this page is how it gets in). */
import GateFrame from '../../components/gate/GateFrame.desktop.vue';
import { useLoginPage } from './access.logic';
import LoginBody from './LoginBody.vue';

const page = useLoginPage();
</script>

<template>
  <GateFrame title="登录设备" sub="输入设备主人设置的访问密码。">
    <LoginBody :page="page" />
  </GateFrame>
</template>
