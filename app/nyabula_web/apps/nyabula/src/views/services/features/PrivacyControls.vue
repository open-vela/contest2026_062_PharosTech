<script setup lang="ts">
import { MdSwitch, UiIcon } from '@nyabula/ui';
import { usePrivacyStore } from '../../../stores/privacy';
import { useVisiblePoll } from '../../../composables/usePageVisible';
defineProps<{ compact?: boolean }>();
const privacy = usePrivacyStore();
const items = [
  { id: 'camera' as const, name: '摄像头', icon: 'camera', hint: '关闭后停止画面、拍照和视觉分析' },
  { id: 'mic' as const, name: '麦克风', icon: 'mic', hint: '关闭后停止收音、语音唤醒和对讲' },
];
useVisiblePoll(() => void privacy.refresh(), 2500);
</script>
<template>
  <div class="privacy-controls" :class="{ compact }">
    <label v-for="item in items" :key="item.id" class="privacy-row">
      <span class="privacy-icon"><UiIcon :name="item.icon" :size="20" /></span>
      <span class="privacy-copy"><strong>{{ item.name }}</strong><small v-if="!compact">{{ item.hint }}</small></span>
      <span v-if="!compact" class="privacy-state">{{ !privacy.state ? '未读取' : privacy.state[item.id] ? '已开启' : '已关闭' }}</span>
      <MdSwitch :model-value="privacy.state?.[item.id] ?? false" :disabled="!privacy.canWrite" :aria-label="item.name" @update:model-value="privacy.set(item.id, $event)" />
    </label>
    <p class="privacy-note" :class="{ error: privacy.error || privacy.readError }" role="status">{{ privacy.error || privacy.readError || privacy.label }}</p>
  </div>
</template>
<style scoped>
.privacy-controls{display:grid;gap:14px}.privacy-row{display:flex;align-items:center;gap:12px;min-height:58px}.privacy-icon{display:grid;place-items:center;width:40px;height:40px;flex:none;border-radius:12px;background:var(--md-secondary-container);color:var(--md-on-secondary-container)}.privacy-copy{display:grid;gap:4px;flex:1;min-width:0}.privacy-copy strong{font-size:15px}.privacy-copy small,.privacy-state,.privacy-note{color:var(--md-on-surface-variant);font-size:12px;line-height:1.5}.privacy-state{flex:none}.privacy-note{margin:0}.error{color:var(--md-error)}.compact{grid-template-columns:repeat(2,minmax(0,1fr));gap:8px 12px}.compact .privacy-row{min-width:0;min-height:40px;gap:6px}.compact .privacy-icon{width:24px;height:28px;border-radius:8px;background:transparent}.compact .privacy-copy strong{font-size:12px;white-space:nowrap}.compact .privacy-note{grid-column:1/-1}
</style>
