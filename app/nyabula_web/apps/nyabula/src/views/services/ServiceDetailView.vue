<script setup lang="ts">
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import Desktop from './ServiceDetailView.desktop.vue';
import Tablet from './ServiceDetailView.tablet.vue';
import Phone from './ServiceDetailView.phone.vue';
</script>

<template>
  <LayoutSwitch :desktop="Desktop" :tablet="Tablet" :phone="Phone" />
</template>
