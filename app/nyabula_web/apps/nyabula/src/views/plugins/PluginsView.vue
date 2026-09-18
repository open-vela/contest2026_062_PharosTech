<script setup lang="ts">
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import PluginsDesktop from './PluginsView.desktop.vue';
import PluginsTablet from './PluginsView.tablet.vue';
import PluginsPhone from './PluginsView.phone.vue';
</script>

<template>
  <LayoutSwitch :desktop="PluginsDesktop" :tablet="PluginsTablet" :phone="PluginsPhone" />
</template>
