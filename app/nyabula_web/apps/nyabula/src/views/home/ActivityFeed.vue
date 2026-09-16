<script setup lang="ts">
import { MdCard, UiIcon } from '@nyabula/ui';
import type { useHomePage } from './home.logic';

const props = defineProps<{ page: ReturnType<typeof useHomePage>; limit?: number }>();
const { activity, ago } = props.page;
</script>

<template>
  <MdCard title="最近动态">
    <ul v-if="activity.length" class="feed">
      <li v-for="a in activity.slice(0, limit ?? 8)" :key="a.id" class="feed-item" :class="a.tone">
        <UiIcon :name="a.icon" :size="16" />
        <span class="feed-text">{{ a.text }}</span>
        <span class="feed-time">{{ ago(a.at) }}</span>
      </li>
    </ul>
    <p v-else class="muted" style="font-size: 13px; margin: 0">连接后这里会显示表情、场景与插件的实时变化。</p>
  </MdCard>
</template>

<style scoped>
.feed { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 6px; }
.feed-item { display: flex; align-items: center; gap: 10px; font-size: 13px; color: var(--md-on-surface); padding: 6px 0; border-bottom: 1px dashed var(--md-outline-variant); }
.feed-item:last-child { border-bottom: none; }
.feed-item :deep(.ui-icon) { color: var(--md-on-surface-variant); }
.feed-item.ok :deep(.ui-icon) { color: var(--md-success); }
.feed-item.warn :deep(.ui-icon) { color: var(--md-warning); }
.feed-text { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.feed-time { font-size: 11.5px; color: var(--md-outline); flex: none; }
</style>
