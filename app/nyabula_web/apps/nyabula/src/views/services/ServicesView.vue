<script setup lang="ts">
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import Desktop from './ServicesView.desktop.vue';
import Tablet from './ServicesView.tablet.vue';
import Phone from './ServicesView.phone.vue';
</script>

<template>
  <LayoutSwitch :desktop="Desktop" :tablet="Tablet" :phone="Phone" />
</template>
