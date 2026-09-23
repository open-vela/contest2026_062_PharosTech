<script setup lang="ts">
import LayoutSwitch from '../../components/LayoutSwitch.vue';
import EyeDesktop from './EyeView.desktop.vue';
import EyeTablet from './EyeView.tablet.vue';
import EyePhone from './EyeView.phone.vue';
</script>

<template>
  <LayoutSwitch :desktop="EyeDesktop" :tablet="EyeTablet" :phone="EyePhone" />
</template>
