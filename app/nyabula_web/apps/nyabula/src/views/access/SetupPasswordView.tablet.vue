<script setup lang="ts">
/* Tablet: brand panel beside the card in landscape, one touch-sized column in portrait. */
import GateFrame from '../../components/gate/GateFrame.tablet.vue';
import { useSetupPasswordPage } from './access.logic';
import SetupPasswordBody from './SetupPasswordBody.vue';

const page = useSetupPasswordPage();
</script>

<template>
  <GateFrame :title="page.title.value" :sub="page.sub.value">
    <SetupPasswordBody :page="page" />
  </GateFrame>
</template>
