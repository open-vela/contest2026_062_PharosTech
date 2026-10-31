<script setup lang="ts">
import SkinPage from '../../components/SkinPage.vue';
import HomeDesktop from './HomeView.desktop.vue';
import HomeTablet from './HomeView.tablet.vue';
import HomePhone from './HomeView.phone.vue';
</script>

<template>
  <SkinPage page="home" :desktop="HomeDesktop" :tablet="HomeTablet" :phone="HomePhone" />
</template>
