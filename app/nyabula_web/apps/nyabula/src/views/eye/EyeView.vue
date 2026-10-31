<script setup lang="ts">
import SkinPage from '../../components/SkinPage.vue';
import EyeDesktop from './EyeView.desktop.vue';
import EyeTablet from './EyeView.tablet.vue';
import EyePhone from './EyeView.phone.vue';
</script>

<template>
  <SkinPage page="eye" :desktop="EyeDesktop" :tablet="EyeTablet" :phone="EyePhone" />
</template>
