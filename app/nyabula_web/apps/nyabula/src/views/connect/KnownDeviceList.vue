<script setup lang="ts">
import { EmptyState, UiIcon } from '@nyabula/ui';
import type { useConnectPage } from './connect.logic';

const props = defineProps<{ page: ReturnType<typeof useConnectPage>; grid?: boolean }>();
const { known, open, forget, session } = props.page;
function ago(t: number): string {
  const m = Math.round((Date.now() - t) / 60000);
  if (m < 1) return '刚刚';
  if (m < 60) return `${m} 分钟前`;
  const h = Math.round(m / 60);
  if (h < 24) return `${h} 小时前`;
  return `${Math.round(h / 24)} 天前`;
}
</script>

<template>
  <div v-if="known.length" class="known" :class="{ grid }">
    <div v-for="d in known" :key="d.key" class="known-card hover-lift" :class="{ current: d.key === session.deviceKey }" @click="open(d.key)">
      <span class="known-icon"><UiIcon :name="d.transport === 'cloud' ? 'cloud' : 'wifi'" :size="22" /></span>
      <span class="known-body">
        <span class="known-title">{{ d.label }}</span>
        <span class="known-sub mono">{{ d.address }}</span>
        <span class="known-meta">{{ d.transport === 'cloud' ? '云中继' : '局域网' }} · {{ ago(d.lastSeen) }}<template v-if="d.coreVersion"> · Core {{ d.coreVersion }}</template></span>
      </span>
      <button class="known-forget" title="忘记" @click.stop="forget(d.key)"><UiIcon name="delete" :size="18" /></button>
    </div>
  </div>
  <EmptyState v-else icon="devices" title="还没有连接过设备" hint="在右侧输入设备局域网 IP，或登录账号从云端选择。" compact />
</template>

<style scoped>
.known { display: flex; flex-direction: column; gap: 10px; }
.known.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); }
.known-card {
  display: flex;
  align-items: center;
  gap: 14px;
  padding: 14px 16px;
  border-radius: var(--radius-l);
  background: var(--md-surface-container);
  cursor: pointer;
  border: 1px solid transparent;
}
.known-card.current { border-color: rgba(var(--md-primary-rgb), 0.5); }
.known-icon {
  width: 44px;
  height: 44px;
  border-radius: 14px;
  display: grid;
  place-items: center;
  background: var(--md-primary-container);
  color: var(--md-on-primary-container);
  flex: none;
}
.known-body { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
.known-title { font: 600 15px var(--font-body); color: var(--md-on-surface); }
.known-sub { font-size: 12.5px; color: var(--md-on-surface-variant); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.known-meta { font-size: 11.5px; color: var(--md-outline); }
.known-forget { border: none; background: transparent; color: var(--md-on-surface-variant); width: 34px; height: 34px; border-radius: 50%; display: grid; place-items: center; cursor: pointer; }
.known-forget:hover { background: var(--md-surface-container-highest); color: var(--md-error); }
</style>
