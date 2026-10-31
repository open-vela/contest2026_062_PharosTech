<script setup lang="ts">
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import PageDesktop from './PluginPageView.desktop.vue';
import PageTablet from './PluginPageView.tablet.vue';
import PagePhone from './PluginPageView.phone.vue';
</script>

<template>
  <LayoutSwitch :desktop="PageDesktop" :tablet="PageTablet" :phone="PagePhone" />
</template>
