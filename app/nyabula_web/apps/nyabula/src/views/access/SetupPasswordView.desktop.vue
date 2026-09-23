<script setup lang="ts">
/* Desktop: centred fixed-width column (a computer cannot scan the QR code, so this page is how it gets in). */
import GateFrame from '../../components/gate/GateFrame.desktop.vue';
import { useSetupPasswordPage } from './access.logic';
import SetupPasswordBody from './SetupPasswordBody.vue';

const page = useSetupPasswordPage();
</script>

<template>
  <GateFrame :title="page.title.value" :sub="page.sub.value">
    <SetupPasswordBody :page="page" />
  </GateFrame>
</template>
