<script setup lang="ts">
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import DeviceDesktop from './DeviceView.desktop.vue';
import DeviceTablet from './DeviceView.tablet.vue';
import DevicePhone from './DeviceView.phone.vue';

/* `key` is reserved by Vue and never reaches props; variants read the route. */
defineProps<{ section?: string }>();
</script>

<template>
  <LayoutSwitch :desktop="DeviceDesktop" :tablet="DeviceTablet" :phone="DevicePhone" :section="section" />
</template>
