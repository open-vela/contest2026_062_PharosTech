<script setup lang="ts">
/* The flow is a single column on every form factor; tablet reuses desktop. */
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import ProvisionDesktop from './ProvisionView.desktop.vue';
import ProvisionPhone from './ProvisionView.phone.vue';
</script>

<template>
  <LayoutSwitch :desktop="ProvisionDesktop" :phone="ProvisionPhone" />
</template>
