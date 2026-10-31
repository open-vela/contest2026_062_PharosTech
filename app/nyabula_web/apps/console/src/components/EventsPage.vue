<script setup lang="ts">
import { computed, ref } from 'vue';
import { MdCard, SegmentedTabs } from '@nyabula/ui';
import { useEventsStore } from '../stores/events';
const events = useEventsStore();
const tone = ref('all');
const query = ref('');
const tabs = [{id:'all',label:'全部'},{id:'ok',label:'成功'},{id:'info',label:'信息'},{id:'warn',label:'警告'}];
const rows = computed(() => events.entries.filter(e => (tone.value === 'all' || e.tone === tone.value) && e.text.toLowerCase().includes(query.value.trim().toLowerCase())));
</script>
<template><div class="page events"><header><h1>事件</h1><p>本次登录后，此浏览器观察到的设备变化与操作结果。保留最近 200 条。</p></header><div class="filters"><SegmentedTabs v-model="tone" :items="tabs" /><input v-model="query" aria-label="搜索事件" placeholder="搜索设备或操作" /></div><MdCard><ol v-if="rows.length"><li v-for="e in rows" :key="e.id"><span class="dot" :class="e.tone" /><span>{{ e.text }}</span><time :datetime="e.at">{{ new Date(e.at).toLocaleString() }}</time></li></ol><p v-else>没有符合条件的事件。</p></MdCard></div></template>
<style scoped>
.events { padding: 24px; max-width: 1100px; margin: auto; display: flex; flex-direction: column; gap: 20px; }h1 { margin: 0; font: 30px var(--font-title); }header p { color: var(--md-on-surface-variant); line-height: 1.6; }.filters { display: flex; flex-wrap: wrap; gap: 14px; align-items: center; }input { padding: 12px; border: 1px solid var(--md-outline-variant); border-radius: var(--radius-m); background: var(--md-surface-container); color: var(--md-on-surface); }ol { list-style: none; padding: 0; margin: 0; }li { display: grid; grid-template-columns: 10px minmax(0,1fr) auto; gap: 14px; padding: 18px 0; border-bottom: 1px solid var(--md-outline-variant); align-items: baseline; }li:last-child { border: 0; }.dot { width: 8px; height: 8px; border-radius: 50%; background: var(--md-primary); }.dot.ok { background: var(--md-success); }.dot.warn { background: var(--md-warning); }time { color: var(--md-on-surface-variant); font-size: 12px; }@media(max-width:600px){.events { padding: 16px; }li { grid-template-columns: 10px minmax(0,1fr); }time { grid-column: 2; }}
</style>
