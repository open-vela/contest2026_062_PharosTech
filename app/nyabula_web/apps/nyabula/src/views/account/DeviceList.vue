<script setup lang="ts">
/* Claimed devices with remote / stats / unclaim actions. `dense` renders
 * list tiles with a trailing menu button (phone) instead of inline buttons. */
import { EmptyState, MdButton, Skeleton, UiIcon, NyabulaLogo } from '@nyabula/ui';
import type { useAccountPage } from './account.logic';

const props = defineProps<{ page: ReturnType<typeof useAccountPage>; dense?: boolean }>();
const emit = defineEmits<{ (e: 'menu', deviceId: string): void; (e: 'claim'): void }>();
const p = props.page;
</script>

<template>
  <div class="stack">
    <Skeleton v-if="p.account.loading && !p.account.devices.length" :lines="3" />
    <EmptyState
      v-else-if="!p.account.devices.length"
      icon="devices"
      title="还没有设备"
      hint="输入设备 ID 与认领码，把 Nyabula 绑定到你的账号"
      action-text="去认领"
      @action="emit('claim')"
    />
    <template v-else>
      <div v-for="d in p.account.devices" :key="d.deviceId" class="list-tile dev">
        <div class="tile-icon" :class="{ on: d.online }"><NyabulaLogo  :size="22" /></div>
        <div class="tile-body">
          <div class="tile-title row">
            <span>{{ d.name || d.deviceId }}</span>
            <span class="tag" :class="d.online ? 'ok' : ''">{{ d.online ? '在线' : '离线' }}</span>
          </div>
          <div class="tile-sub mono">{{ d.deviceId }} · core {{ d.coreVersion || '?' }} · 最近 {{ p.formatTime(d.lastSeen) }}</div>
        </div>
        <div v-if="dense" class="tile-trail">
          <MdButton variant="icon" @click="emit('menu', d.deviceId)"><UiIcon name="more_vert" :size="20" /></MdButton>
        </div>
        <div v-else class="tile-trail actions">
          <MdButton variant="tonal" :disabled="!d.online" @click="p.openRemote(d.deviceId)"><UiIcon name="link" :size="16" /> 远程连接</MdButton>
          <MdButton variant="outlined" @click="p.openStats(d.deviceId)"><UiIcon name="dashboard" :size="16" /> 统计</MdButton>
          <MdButton variant="text" @click="p.unclaim(d.deviceId, d.name)"><UiIcon name="link_off" :size="16" /> 解绑</MdButton>
        </div>
      </div>
    </template>
  </div>
</template>

<style scoped>
.dev { cursor: default; }
.dev:active { transform: none; }
.tile-icon.on { background: var(--md-primary-container); color: var(--md-on-primary-container); }
.actions { gap: 6px; flex-wrap: wrap; justify-content: flex-end; }
</style>
