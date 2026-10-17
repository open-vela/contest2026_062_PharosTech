<script setup lang="ts">
/* Storage: storage.status (volumes + per-directory usage) and
 * storage.cleanup {target} (owner only, confirmed first). */
import { ref } from 'vue';
import { EmptyState, MdButton, MdCard, Skeleton, UiIcon, useDialogStore, useToastStore } from '@nyabula/ui';
import { fmtBytes, isUnsupportedError, parseStorageStatus, type StorageUsage } from '../../../lib/deviceMaint';
import { useDeviceTopic } from './deviceTopic';

/** Deleting a large directory on flash can take a while. */
const CLEANUP_TIMEOUT_MS = 30000;

const dialog = useDialogStore();
const toast = useToastStore();
const { session, data: status, busy, unsupported, run } = useDeviceTopic('storage.status', parseStorageStatus, '读取存储状态失败');

const cleaning = ref<string | null>(null);
async function cleanup(item: StorageUsage): Promise<void> {
  if (!session.isOwner || cleaning.value) return;
  const ok = await dialog.confirm(`将删除「${item.label}」${item.path ? `（${item.path}）` : ''}中的内容，当前占用 ${fmtBytes(item.bytes)}。此操作无法撤销。`, { title: '清理这些数据？', danger: true, confirmText: '清理' });
  if (!ok) return;
  cleaning.value = item.id;
  try {
    const next = parseStorageStatus(await session.request('storage.cleanup', { target: item.id }, { timeoutMs: CLEANUP_TIMEOUT_MS }));
    // The answer is a fresh storage.status; keep the old view if it came back empty.
    if (next.volumes.length || next.usage.length) status.value = next;
    else void run();
    toast.ok(next.freed !== null ? `已清理「${item.label}」，释放 ${fmtBytes(next.freed)}` : `已清理「${item.label}」`);
  } catch (e) {
    if (isUnsupportedError(e)) toast.show('此固件不支持清理该项目', 'error');
    else toast.error(e, '清理失败');
  } finally {
    cleaning.value = null;
  }
}
</script>

<template>
  <div class="stack">
    <MdCard v-if="unsupported">
      <EmptyState compact icon="storage" title="此固件不支持" hint="设备固件未提供存储状态（storage.status），升级固件后可用。" />
    </MdCard>
    <template v-else>
      <MdCard title="存储卷">
        <Skeleton v-if="busy && !status" :lines="4" />
        <EmptyState v-else-if="!status" tone="error" compact title="读取失败" hint="设备未响应 storage.status" action-text="重试" @action="run()" />
        <EmptyState v-else-if="!status.volumes.length" compact icon="storage" title="没有可显示的存储卷" />
        <div v-else class="volumes">
          <div v-for="v in status.volumes" :key="v.id" class="volume">
            <div class="vol-head">
              <span class="vol-icon"><UiIcon name="storage" :size="20" /></span>
              <div class="vol-id">
                <div class="vol-label" :title="v.label">{{ v.label }}</div>
                <div v-if="v.path" class="vol-path mono muted" :title="v.path">{{ v.path }}</div>
              </div>
              <span class="tag" :class="v.percent >= 90 ? 'err' : v.percent >= 75 ? 'warn' : 'ok'">{{ v.total ? v.percent + '%' : '—' }}</span>
            </div>
            <div class="bar" role="progressbar" aria-valuemin="0" aria-valuemax="100" :aria-valuenow="v.percent" :aria-label="`${v.label} 已用 ${v.percent}%`">
              <span :style="{ width: v.percent + '%' }" :class="{ warn: v.percent >= 75, full: v.percent >= 90 }" />
            </div>
            <div class="vol-nums muted">
              <span>已用 {{ fmtBytes(v.used) }}</span>
              <span>可用 {{ fmtBytes(v.free) }}</span>
              <span>共 {{ fmtBytes(v.total) }}</span>
            </div>
          </div>
        </div>
        <div v-if="status" class="row" style="justify-content: flex-end; margin-top: 12px">
          <MdButton variant="text" :disabled="busy" @click="run()"><UiIcon name="refresh" :size="16" /> {{ busy ? '刷新中…' : '刷新' }}</MdButton>
        </div>
      </MdCard>

      <MdCard v-if="status" title="目录占用">
        <p v-if="!status.usage.length" class="muted empty-line">设备没有报告目录占用。</p>
        <div v-else class="usage">
          <div v-for="u in status.usage" :key="u.id" class="usage-row">
            <div class="usage-id">
              <div class="usage-label" :title="u.label">{{ u.label }}</div>
              <div v-if="u.path" class="usage-path mono muted" :title="u.path">{{ u.path }}</div>
            </div>
            <span class="usage-size mono">{{ fmtBytes(u.bytes) }}</span>
            <MdButton v-if="u.clearable" variant="tonal" class="usage-act" :disabled="!session.isOwner || cleaning !== null || u.bytes === 0" @click="cleanup(u)">
              {{ cleaning === u.id ? '清理中…' : '清理' }}
            </MdButton>
            <span v-else class="usage-act keep muted" title="系统数据，不能在这里清理">不可清理</span>
          </div>
        </div>
        <p v-if="status.usage.some((u) => u.clearable) && !session.isOwner" class="muted hint">仅主人 (owner) 可以清理数据。</p>
      </MdCard>
    </template>
  </div>
</template>

<style scoped>
.volumes { display: flex; flex-direction: column; gap: 18px; }
.vol-head { display: flex; align-items: center; gap: 12px; min-width: 0; }
.vol-icon {
  width: 40px;
  height: 40px;
  border-radius: 12px;
  display: grid;
  place-items: center;
  background: var(--md-secondary-container);
  color: var(--md-on-secondary-container);
  flex: none;
}
.vol-id, .usage-id { flex: 1; min-width: 0; }
.vol-label, .usage-label { font: 600 14.5px var(--font-body); color: var(--md-on-surface); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.vol-path, .usage-path { font-size: 12px; margin-top: 2px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.bar { margin-top: 10px; height: 8px; border-radius: 999px; background: var(--md-surface-container-highest); overflow: hidden; }
.bar span { display: block; height: 100%; min-width: 2px; background: var(--md-primary); border-radius: 999px; transition: width var(--dur); }
.bar span.warn { background: var(--md-warning); }
.bar span.full { background: var(--md-error); }
.vol-nums { display: flex; flex-wrap: wrap; justify-content: space-between; gap: 4px 14px; font-size: 12.5px; margin-top: 8px; }
.usage { display: flex; flex-direction: column; }
.usage-row { display: flex; align-items: center; gap: 12px; padding: 10px 0; border-bottom: 1px solid var(--md-outline-variant); min-width: 0; }
.usage-row:last-child { border-bottom: none; }
.usage-size { flex: none; font-size: 13px; font-weight: 600; color: var(--md-on-surface); }
.usage-act { flex: none; min-width: 76px; text-align: center; }
.usage-act.md-btn { padding: 7px 14px; font-size: 13px; }
.usage-act.keep { font-size: 12px; }
.empty-line { font-size: 13px; margin: 0; }
.hint { font-size: 12px; margin: 10px 0 0; }
.md-btn :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
</style>
