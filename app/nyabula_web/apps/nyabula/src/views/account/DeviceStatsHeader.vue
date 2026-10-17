<script setup lang="ts">
/* Current status strip + totals for one device. */
import { MdButton, MdCard, UiIcon } from '@nyabula/ui';
import type { useAccountDevicePage } from './accountDevice.logic';
import { formatDuration } from './accountDevice.logic';

const props = defineProps<{ page: ReturnType<typeof useAccountDevicePage>; compact?: boolean }>();
const p = props.page;
</script>

<template>
  <div class="head" :class="{ compact }">
    <MdCard class="cur">
      <div class="row between">
        <div>
          <div class="muted lbl">当前状态</div>
          <div class="row" style="margin-top: 4px">
            <span class="tag" :class="p.stats.value?.current.online ? 'ok' : ''">{{ p.stats.value?.current.online ? '在线' : '离线' }}</span>
            <span class="val">{{ p.stats.value?.current.clients ?? 0 }} <span class="muted unit">客户端</span></span>
          </div>
        </div>
        <MdButton variant="tonal" :disabled="!p.stats.value?.current.online" @click="p.openRemote()"><UiIcon name="link" :size="16" /> 远程连接</MdButton>
      </div>
    </MdCard>
    <MdCard class="tot">
      <div class="muted lbl">合计在线</div>
      <div class="val">{{ formatDuration(p.totals.value.onlineSeconds) }}</div>
    </MdCard>
    <MdCard class="tot">
      <div class="muted lbl">帧 上行 / 下行</div>
      <div class="val mono">{{ p.totals.value.framesUp.toLocaleString() }} / {{ p.totals.value.framesDown.toLocaleString() }}</div>
    </MdCard>
  </div>
</template>

<style scoped>
.head { display: grid; grid-template-columns: 2fr 1fr 1fr; gap: 12px; }
.head.compact { grid-template-columns: 1fr 1fr; }
.head.compact .cur { grid-column: 1 / -1; }
.lbl { font-size: 12px; }
.val { font: 700 20px var(--font-title); color: var(--md-on-surface); }
.unit { font: 500 12px var(--font-body); }
</style>
