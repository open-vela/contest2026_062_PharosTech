<script setup lang="ts">
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import SettingsDesktop from './SettingsView.desktop.vue';
import SettingsTablet from './SettingsView.tablet.vue';
import SettingsPhone from './SettingsView.phone.vue';
</script>

<template>
  <LayoutSwitch :desktop="SettingsDesktop" :tablet="SettingsTablet" :phone="SettingsPhone" />
</template>
