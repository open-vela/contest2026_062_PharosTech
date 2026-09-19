<script setup lang="ts">
/* Sticky reconnect / offline banner shown inside the device workspace. */
import { computed } from 'vue';
import { useRoute } from 'vue-router';
import { UiIcon } from '@nyabula/ui';
import { useSessionStore } from '../stores/session';
import { isPreviewKey } from '../stores/dev';

const session = useSessionStore();
const route = useRoute();
const preview = computed(() => isPreviewKey(session.deviceKey));
/* Routes flagged `quietLink` (WiFi provisioning) expect the link to drop and explain it themselves. */
const show = computed(() => !route.meta.quietLink && (preview.value || (session.deviceKey && (session.state === 'reconnecting' || session.state === 'closed' || session.state === 'connecting'))));
const text = computed(() =>
  preview.value ? '开发预览：未连接设备，页面以离线/空态展示' : session.authRequired ? (session.lastError ?? '认证失败，请重新输入设备令牌') : session.state === 'reconnecting' ? '连接丢失，正在重连… 设备任务可能仍在执行' : session.state === 'connecting' ? '正在连接设备…' : '连接已断开',
);
</script>

<template>
  <Transition name="fade">
    <div v-if="show" class="banner" :class="preview ? 'preview' : session.state">
      <UiIcon :name="preview ? 'terminal' : session.state === 'closed' ? 'link_off' : 'sync'" :size="18" :class="{ spin: !preview && session.state !== 'closed' }" />
      <span>{{ text }}</span>
      <RouterLink v-if="session.authRequired" class="retry" :to="{ path: '/connect', query: { stay: '1', reauth: session.deviceKey ?? undefined } }">重新认证</RouterLink>
      <button v-else-if="!preview && session.state === 'closed' && session.deviceKey" class="retry" @click="session.connect(session.deviceKey)">重试</button>
    </div>
  </Transition>
</template>

<style scoped>
.banner {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 8px 16px;
  font-size: 13px;
  background: color-mix(in srgb, var(--md-warning) 18%, var(--md-surface-container));
  color: var(--md-on-surface);
  border-bottom: 1px solid color-mix(in srgb, var(--md-warning) 40%, transparent);
}
.banner.preview { background: color-mix(in srgb, var(--md-tertiary) 16%, var(--md-surface-container)); border-bottom-color: color-mix(in srgb, var(--md-tertiary) 40%, transparent); }
.banner.closed { background: color-mix(in srgb, var(--md-error) 16%, var(--md-surface-container)); }
.spin { animation: spin 1.2s linear infinite; }
@keyframes spin { to { transform: rotate(360deg); } }
.retry {
  margin-left: auto;
  border: none;
  background: var(--md-primary);
  color: var(--md-on-primary);
  border-radius: 999px;
  padding: 4px 12px;
  font: 600 12px var(--font-body);
  cursor: pointer;
}
</style>
