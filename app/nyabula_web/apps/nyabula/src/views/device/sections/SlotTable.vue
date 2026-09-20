<script setup lang="ts">
/* The A/B table of one boot domain (NuttX or AMP), as bootctrl has it. */
import { UiIcon } from '@nyabula/ui';
import { fmtBytes, type UpdateSlot } from '../../../lib/deviceMaint';

defineProps<{ slots: UpdateSlot[] }>();

function slotState(s: UpdateSlot): { text: string; tone: string } {
  if (s.running) return { text: '正在运行', tone: 'ok' };
  if (!s.size) return { text: '空', tone: 'info' };
  if (!s.bootable) return { text: '不可启动', tone: 'err' };
  if (s.active) return { text: '下次启动', tone: 'warn' };
  return s.successful ? { text: '备用', tone: 'info' } : { text: '待验证', tone: 'warn' };
}
</script>

<template>
  <p v-if="!slots.length" class="muted line">设备没有报告这一组 A/B 槽位的信息。</p>
  <div v-else class="slots-scroll">
    <table class="slots">
      <thead>
        <tr><th>槽位</th><th>状态</th><th>可启动</th><th>启动已确认</th><th>版本序号</th><th>大小</th></tr>
      </thead>
      <tbody>
        <tr v-for="s in slots" :key="s.name" :class="{ on: s.running }">
          <td class="slot-name">{{ s.name.toUpperCase() }}</td>
          <td><span class="tag" :class="slotState(s).tone">{{ slotState(s).text }}</span></td>
          <td><UiIcon :name="s.bootable ? 'check_circle' : 'cancel'" :size="18" :class="s.bootable ? 'yes' : 'no'" /><span class="sr">{{ s.bootable ? '是' : '否' }}</span></td>
          <td><UiIcon :name="s.successful ? 'check_circle' : 'cancel'" :size="18" :class="s.successful ? 'yes' : 'no'" /><span class="sr">{{ s.successful ? '是' : '否' }}</span></td>
          <td class="mono">{{ s.size ? s.version : '—' }}</td>
          <td class="mono">{{ s.size ? fmtBytes(s.size) : '—' }}</td>
        </tr>
      </tbody>
    </table>
  </div>
</template>

<style scoped>
.line { font-size: 13px; margin: 0; line-height: 1.6; }
.slots-scroll { overflow-x: auto; }
.slots { width: 100%; border-collapse: collapse; font-size: 13.5px; }
.slots th { text-align: left; font-weight: 600; font-size: 12px; color: var(--md-on-surface-variant); padding: 0 12px 8px 0; white-space: nowrap; }
.slots td { padding: 10px 12px 10px 0; border-top: 1px solid var(--md-outline-variant); color: var(--md-on-surface); white-space: nowrap; vertical-align: middle; }
.slots th:last-child, .slots td:last-child { padding-right: 0; text-align: right; }
.slot-name { font: 600 15px var(--font-title); }
.slots tr.on .slot-name { color: var(--md-primary); }
.slots .ui-icon { vertical-align: middle; }
.yes { color: var(--md-success); }
.no { color: var(--md-on-surface-variant); opacity: 0.6; }
.sr { position: absolute; width: 1px; height: 1px; overflow: hidden; clip: rect(0 0 0 0); white-space: nowrap; }
</style>
