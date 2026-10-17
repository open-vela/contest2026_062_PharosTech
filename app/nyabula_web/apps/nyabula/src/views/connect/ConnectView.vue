<script setup lang="ts">
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import ConnectDesktop from './ConnectView.desktop.vue';
import ConnectPhone from './ConnectView.phone.vue';
</script>

<template>
  <LayoutSwitch :desktop="ConnectDesktop" :phone="ConnectPhone" />
</template>
