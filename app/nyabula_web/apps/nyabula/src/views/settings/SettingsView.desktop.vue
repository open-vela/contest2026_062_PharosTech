<script setup lang="ts">
/* Desktop: centred page; the skin picker spans the width (three previews
 * side by side), the smaller cards pair up underneath. */
import SettingsCards from './SettingsCards.vue';
import { useSettingsPage } from './settings.logic';

const page = useSettingsPage();
</script>

<template>
  <div class="page narrow settings-desktop">
    <h1 class="page-title">设置</h1>
    <p class="page-sub">仅影响本浏览器，不会同步到设备。</p>
    <div class="stack col">
      <SettingsCards :page="page" :only="['skin']" />
      <div class="cols">
        <div class="stack col"><SettingsCards :page="page" :only="['theme', 'layout']" /></div>
        <div class="stack col"><SettingsCards :page="page" :only="['palette', 'dev', 'data']" /></div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.settings-desktop { max-width: 920px; padding-top: 24px; }
.cols { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; align-items: start; }
.col { gap: 16px; }
</style>
