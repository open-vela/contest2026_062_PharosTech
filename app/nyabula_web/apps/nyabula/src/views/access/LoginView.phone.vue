<script setup lang="ts">
/* Phone: compact brand row, everything top-aligned above the keyboard; the submit button sits right under the field. */
import GateFrame from '../../components/gate/GateFrame.phone.vue';
import { useLoginPage } from './access.logic';
import LoginBody from './LoginBody.vue';

const page = useLoginPage();
</script>

<template>
  <GateFrame title="登录设备" sub="输入设备主人设置的访问密码。">
    <LoginBody :page="page" thumb />
  </GateFrame>
</template>
