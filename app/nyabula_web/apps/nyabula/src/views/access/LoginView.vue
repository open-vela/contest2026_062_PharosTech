<script setup lang="ts">
/* Entry: one variant per form factor (the frame around the form differs). */
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import LoginDesktop from './LoginView.desktop.vue';
import LoginTablet from './LoginView.tablet.vue';
import LoginPhone from './LoginView.phone.vue';
</script>

<template>
  <LayoutSwitch :desktop="LoginDesktop" :tablet="LoginTablet" :phone="LoginPhone" />
</template>
