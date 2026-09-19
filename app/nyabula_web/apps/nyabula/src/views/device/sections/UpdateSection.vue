<script setup lang="ts">
/* Firmware: update.status readout (running version, A/B slots). This
 * firmware has no online update service, so there is nothing to trigger from
 * here: the page says how updates arrive instead of offering dead buttons. */
import { computed } from 'vue';
import { EmptyState, MdButton, MdCard, Skeleton, UiIcon } from '@nyabula/ui';
import { parseUpdateStatus, type UpdateSlot } from '../../../lib/deviceMaint';
import { useDeviceTopic } from './deviceTopic';

const { data: status, busy, unsupported, run } = useDeviceTopic('update.status', parseUpdateStatus, '读取固件状态失败');

const CHANNEL_ZH: Record<string, string> = { manual: '手动（OTA 包 / USB）', stable: '稳定版', beta: '测试版' };
const channel = computed(() => (status.value ? CHANNEL_ZH[status.value.channel] ?? status.value.channel : '—'));
const slotLabel = (name: string): string => (name ? `槽位 ${name.toUpperCase()}` : '未知');

function slotState(s: UpdateSlot): { text: string; tone: string } {
  if (s.active) return { text: '正在运行', tone: 'ok' };
  if (!s.bootable) return { text: '不可启动', tone: 'err' };
  return s.successful ? { text: '备用', tone: 'info' } : { text: '待验证', tone: 'warn' };
}
</script>

<template>
  <div class="stack">
    <MdCard v-if="unsupported">
      <EmptyState compact icon="download" title="此固件不支持" hint="设备固件未提供固件状态（update.status），升级固件后可用。" />
    </MdCard>
    <template v-else>
      <MdCard title="当前固件">
        <Skeleton v-if="busy && !status" :lines="4" />
        <EmptyState v-else-if="!status" tone="error" compact title="读取失败" hint="设备未响应 update.status" action-text="重试" @action="run()" />
        <template v-else>
          <dl class="kv">
            <div><dt>版本</dt><dd class="mono">{{ status.current.version || '—' }}</dd></div>
            <div><dt>构建时间</dt><dd class="mono">{{ status.current.builtAt || '—' }}</dd></div>
            <div><dt>运行槽位</dt><dd>{{ status.current.slot ? slotLabel(status.current.slot) : '未知（非 A/B 启动）' }}</dd></div>
            <div><dt>更新方式</dt><dd>{{ channel }}</dd></div>
          </dl>
          <div class="row" style="justify-content: flex-end; margin-top: 12px">
            <MdButton variant="text" :disabled="busy" @click="run()"><UiIcon name="refresh" :size="16" /> {{ busy ? '刷新中…' : '刷新' }}</MdButton>
          </div>
        </template>
      </MdCard>

      <MdCard v-if="status" title="A/B 槽位">
        <p v-if="!status.slots.length" class="muted line">设备没有报告 A/B 槽位信息。</p>
        <div v-else class="slots-scroll">
          <table class="slots">
            <thead>
              <tr><th>槽位</th><th>状态</th><th>可启动</th><th>启动已确认</th><th>剩余尝试</th></tr>
            </thead>
            <tbody>
              <tr v-for="s in status.slots" :key="s.name" :class="{ on: s.active }">
                <td class="slot-name">{{ s.name.toUpperCase() }}</td>
                <td><span class="tag" :class="slotState(s).tone">{{ slotState(s).text }}</span></td>
                <td><UiIcon :name="s.bootable ? 'check_circle' : 'cancel'" :size="18" :class="s.bootable ? 'yes' : 'no'" /><span class="sr">{{ s.bootable ? '是' : '否' }}</span></td>
                <td><UiIcon :name="s.successful ? 'check_circle' : 'cancel'" :size="18" :class="s.successful ? 'yes' : 'no'" /><span class="sr">{{ s.successful ? '是' : '否' }}</span></td>
                <td class="mono">{{ s.triesRemaining ?? '—' }}</td>
              </tr>
            </tbody>
          </table>
        </div>
        <p class="muted line" style="margin-top: 12px">新固件总是写入没有在运行的那个槽位；新槽位启动成功并确认后才会成为常用槽位，否则在尝试次数用完后自动回到原来的槽位。</p>
      </MdCard>

      <MdCard v-if="status" title="如何更新">
        <div class="notice">
          <UiIcon name="info" :size="20" />
          <div>
            <p><strong>此固件没有在线更新服务。</strong>设备不会自己检查或下载新版本，所以这里没有「检查更新 / 立即升级」按钮。</p>
            <p>新固件通过 OTA 升级包，或经 USB 连接电脑刷入；刷入后设备从另一个槽位启动，上面的表格会随之变化。</p>
            <p v-if="status.detail" class="detail mono">{{ status.detail }}</p>
          </div>
        </div>
      </MdCard>
    </template>
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
.notice { display: flex; align-items: flex-start; gap: 12px; color: var(--md-on-surface-variant); }
.notice > .ui-icon { flex: none; margin-top: 2px; color: var(--md-primary); }
.notice p { margin: 0 0 8px; font-size: 13.5px; line-height: 1.65; color: var(--md-on-surface); }
.notice p:last-child { margin-bottom: 0; }
.notice .detail { font-size: 12.5px; color: var(--md-on-surface-variant); word-break: break-word; }
.md-btn :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
</style>
