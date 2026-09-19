<script setup lang="ts">
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import Desktop from './AccountDeviceView.desktop.vue';
import Tablet from './AccountDeviceView.tablet.vue';
import Phone from './AccountDeviceView.phone.vue';

defineProps<{ id: string }>();
</script>

<template>
  <LayoutSwitch :desktop="Desktop" :tablet="Tablet" :phone="Phone" />
</template>
