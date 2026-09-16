<script setup lang="ts">
/* Tablet: single column of cards. */
import SettingsCards from './SettingsCards.vue';
import { useSettingsPage } from './settings.logic';

const page = useSettingsPage();
</script>

<template>
  <div class="page narrow settings-tablet">
    <h1 class="page-title">设置</h1>
    <p class="page-sub">仅影响本浏览器，不会同步到设备。</p>
    <div class="stack col"><SettingsCards :page="page" /></div>
  </div>
</template>

<style scoped>
.settings-tablet { max-width: 680px; }
.col { gap: 14px; }
</style>
