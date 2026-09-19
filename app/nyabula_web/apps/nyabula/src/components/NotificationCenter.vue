<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';
import { useSessionStore } from '../stores/session';

interface Notice { id: string; source: string; title: string; at: number; read: boolean }
const session = useSessionStore();
const available = computed(() => session.connected && session.client?.capabilities.includes('core.notifications-v1') === true);
const items = ref<Notice[]>([]);
const open = ref(false);
const error = ref('');
const unread = computed(() => items.value.filter(item => !item.read).length);
let generation = 0;
let poll: ReturnType<typeof setInterval> | undefined;
let pending = false;
watch(() => [session.client, session.state], () => {
  generation++; items.value = []; error.value = ''; pending = false;
  if (available.value) void refresh();
});
async function refresh(): Promise<void> {
  if (!available.value || pending) return;
  const current = generation;
  pending = true;
  try {
    const result = await session.request('notification.list');
    if (current !== generation) return;
    items.value = Array.isArray(result.items) ? result.items as Notice[] : [];
    error.value = '';
  } catch (e) { if (current === generation) error.value = e instanceof Error ? e.message : String(e); }
  finally { if (current === generation) pending = false; }
}
async function read(id: string): Promise<void> {
  const current = generation;
  try { await session.request('notification.read', { id }); if (current === generation) await refresh(); }
  catch (e) { if (current === generation) error.value = e instanceof Error ? e.message : String(e); }
}
onMounted(() => { void refresh(); poll = setInterval(() => void refresh(), 10000); });
onUnmounted(() => clearInterval(poll));
</script>

<template>
  <aside v-if="available" class="notice-center">
    <section v-if="open" id="device-notices" class="notice-panel" aria-label="设备通知中心">
      <header><strong>设备通知</strong><MdButton variant="text" @click="open = false">关闭</MdButton></header>
      <p v-if="error" role="alert">{{ error }}</p><p v-if="!items.length">还没有通知</p>
      <article v-for="item in [...items].reverse()" :key="item.id" :class="{ unread:!item.read }">
        <p>{{ item.title }}</p><small>{{ new Date(item.at).toLocaleString() }}</small>
        <MdButton v-if="!item.read" variant="text" @click="read(item.id)">标为已读</MdButton>
      </article>
    </section>
    <button class="notice-toggle" type="button" aria-controls="device-notices" :aria-expanded="open"
      :aria-label="'设备通知，' + unread + ' 条未读'" @click="open = !open; refresh()">
      <UiIcon name="notifications" :size="20" /><span>通知{{ unread ? ' · ' + unread : '' }}</span>
    </button>
  </aside>
</template>

<style scoped>
.notice-center { position:fixed; z-index:90; right:16px; bottom:calc(var(--shell-bottom, 0px) + var(--safe-b, 0px) + 16px); display:flex; flex-direction:column; align-items:flex-end; gap:8px; }
.notice-toggle { min-height:44px; display:flex; gap:8px; align-items:center; border:0; padding:0 16px; border-radius:var(--radius-full); background:var(--md-secondary-container); color:var(--md-on-secondary-container); cursor:pointer; font:inherit; }
.notice-panel { width:min(360px, calc(100vw - 32px)); max-height:60dvh; overflow:auto; padding:16px; background:var(--md-surface-container-high); border-radius:var(--radius-l); box-shadow:0 8px 28px #0003; }
header { display:flex; align-items:center; justify-content:space-between; }article { padding:12px 0; border-bottom:1px solid var(--md-outline-variant); }article.unread { border-left:3px solid var(--md-primary); padding-left:10px; }p { margin:4px 0; }small { color:var(--md-on-surface-variant); }
</style>
