<script setup lang="ts">
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import AccountDesktop from './AccountView.desktop.vue';
import AccountTablet from './AccountView.tablet.vue';
import AccountPhone from './AccountView.phone.vue';
</script>

<template>
  <LayoutSwitch :desktop="AccountDesktop" :tablet="AccountTablet" :phone="AccountPhone" />
</template>
