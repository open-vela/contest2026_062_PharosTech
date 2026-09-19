<script setup lang="ts">
/* Desktop: centred narrow page, two columns of cards. */
import SettingsCards from './SettingsCards.vue';
import { useSettingsPage } from './settings.logic';

const page = useSettingsPage();
</script>

<template>
  <div class="page narrow settings-desktop">
    <h1 class="page-title">设置</h1>
    <p class="page-sub">仅影响本浏览器，不会同步到设备。</p>
    <div class="cols">
      <div class="stack col"><SettingsCards :page="page" :only="['theme', 'palette']" /></div>
      <div class="stack col"><SettingsCards :page="page" :only="['layout', 'dev', 'data']" /></div>
    </div>
  </div>
</template>

<style scoped>
.settings-desktop { max-width: 900px; }
.cols { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; align-items: start; }
.col { gap: 16px; }
</style>
