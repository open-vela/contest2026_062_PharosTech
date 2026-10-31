<script setup lang="ts">
import SkinPage from '../../components/SkinPage.vue';
import ConnectDesktop from './ConnectView.desktop.vue';
import ConnectPhone from './ConnectView.phone.vue';
</script>

<template>
  <SkinPage page="connect" :desktop="ConnectDesktop" :phone="ConnectPhone" />
</template>
