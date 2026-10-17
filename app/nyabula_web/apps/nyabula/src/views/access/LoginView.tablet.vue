<script setup lang="ts">
/* Tablet: brand panel beside the card in landscape, one touch-sized column in portrait. */
import GateFrame from '../../components/gate/GateFrame.tablet.vue';
import { useLoginPage } from './access.logic';
import LoginBody from './LoginBody.vue';

const page = useLoginPage();
</script>

<template>
  <GateFrame title="登录设备" sub="输入设备主人设置的访问密码。">
    <LoginBody :page="page" />
  </GateFrame>
</template>
