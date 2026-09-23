<script setup lang="ts">
/* The flow is a single column everywhere; the frame around it differs per
 * form factor (device build: bare gate frames; hosted: inside the shell). */
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import ProvisionDesktop from './ProvisionView.desktop.vue';
import ProvisionTablet from './ProvisionView.tablet.vue';
import ProvisionPhone from './ProvisionView.phone.vue';
</script>

<template>
  <LayoutSwitch :desktop="ProvisionDesktop" :tablet="ProvisionTablet" :phone="ProvisionPhone" />
</template>
