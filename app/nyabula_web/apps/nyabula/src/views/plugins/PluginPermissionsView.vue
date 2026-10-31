<script setup lang="ts">
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import PermsDesktop from './PluginPermissionsView.desktop.vue';
import PermsTablet from './PluginPermissionsView.tablet.vue';
import PermsPhone from './PluginPermissionsView.phone.vue';
</script>

<template>
  <LayoutSwitch :desktop="PermsDesktop" :tablet="PermsTablet" :phone="PermsPhone" />
</template>
