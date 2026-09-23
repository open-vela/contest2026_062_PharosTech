<script setup lang="ts">
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import HomeDesktop from './HomeView.desktop.vue';
import HomeTablet from './HomeView.tablet.vue';
import HomePhone from './HomeView.phone.vue';
</script>

<template>
  <LayoutSwitch :desktop="HomeDesktop" :tablet="HomeTablet" :phone="HomePhone" />
</template>
