<script setup lang="ts">
import { FeaturePageHeader } from '../featureHeader';
import PrivacyControls from './PrivacyControls.vue';
import type { FeatureProps } from './contract';
defineProps<FeatureProps>();
</script>
<template>
 <div class="privacy-feature">
  <FeaturePageHeader icon="privacy" title="隐私" subtitle="摄像头与麦克风的软件开关" />
  <section class="ui-panel privacy-panel"><PrivacyControls /></section>
 </div>
</template>
<style scoped>
.privacy-feature{display:grid;gap:16px}.privacy-panel{padding:20px;max-width:720px;width:100%;box-sizing:border-box}
</style>
