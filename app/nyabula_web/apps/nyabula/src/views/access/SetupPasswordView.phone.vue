<script setup lang="ts">
/* Phone: compact brand row, everything top-aligned above the keyboard; the submit button sits right under the field. */
import GateFrame from '../../components/gate/GateFrame.phone.vue';
import { useSetupPasswordPage } from './access.logic';
import SetupPasswordBody from './SetupPasswordBody.vue';

const page = useSetupPasswordPage();
</script>

<template>
  <GateFrame :title="page.title.value" :sub="page.sub.value">
    <SetupPasswordBody :page="page" thumb />
  </GateFrame>
</template>
