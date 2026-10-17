<script setup lang="ts">
/* Entry: one variant per form factor (the frame around the form differs). */
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import SetupDesktop from './SetupPasswordView.desktop.vue';
import SetupTablet from './SetupPasswordView.tablet.vue';
import SetupPhone from './SetupPasswordView.phone.vue';
</script>

<template>
  <LayoutSwitch :desktop="SetupDesktop" :tablet="SetupTablet" :phone="SetupPhone" />
</template>
