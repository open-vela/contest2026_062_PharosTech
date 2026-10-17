<script setup lang="ts">
/* Cloud relay: cloud.status readout + cloud.config form (owner only).
 * Device contract: {enabled,url,state,detail}; the earlier
 * {enabled,url,connected,deviceId,claimCode} shape is still understood
 * (parseCloudStatus). state "unsupported" = the relay client is not part of
 * this firmware; the settings are stored all the same. */
import { computed, ref, watch } from 'vue';
import { EmptyState, MdButton, MdCard, MdSwitch, MdTextField, Skeleton, UiIcon, useToastStore } from '@nyabula/ui';
import { useSessionStore } from '../../../stores/session';
import { useAsyncTask } from '../../../composables/useRequest';
import { copyText } from '../../../lib/clipboard';
import { parseCloudStatus, type CloudState, type CloudStatus } from '../../../lib/deviceMaint';

const session = useSessionStore();
const toast = useToastStore();
const status = ref<CloudStatus | null>(null);
const task = useAsyncTask(async () => {
  status.value = parseCloudStatus(await session.request('cloud.status'));
  return true;
}, { errorPrefix: '读取云中继状态失败', holdRoute: true, immediate: true });

const STATE_META: Record<CloudState, { title: string; tag: string; tone: string; icon: string }> = {
  disabled: { title: '已停用', tag: '停用', tone: 'info', icon: 'cloud_off' },
  offline: { title: '未连接', tag: '离线', tone: 'warn', icon: 'cloud_off' },
  connecting: { title: '正在连接云端…', tag: '连接中', tone: 'warn', icon: 'sync' },
  online: { title: '已连接云端', tag: '在线', tone: 'ok', icon: 'cloud' },
  unsupported: { title: '此固件不含云中继', tag: '不支持', tone: 'info', icon: 'cloud_off' },
};
const meta = computed(() => STATE_META[status.value?.state ?? 'disabled']);
const unsupported = computed(() => status.value?.state === 'unsupported');
const deviceId = computed(() => status.value?.deviceId ?? session.device?.id ?? null);

const enabled = ref(false);
const url = ref('');
watch(status, (s) => {
  if (!s) return;
  enabled.value = s.enabled;
  url.value = s.url;
}, { immediate: true });
const dirty = computed(() => !!status.value && (enabled.value !== status.value.enabled || url.value.trim() !== status.value.url));
const urlError = computed(() => (url.value.trim() && !/^wss?:\/\/\S+$/.test(url.value.trim()) ? '需以 ws:// 或 wss:// 开头' : null));

const saving = ref(false);
async function save(): Promise<void> {
  if (!session.isOwner || urlError.value) return;
  saving.value = true;
  try {
    const raw = await session.request('cloud.config', { enabled: enabled.value, url: url.value.trim() });
    // The device answers with the new status; an older Core answers {} -> read it back.
    if (typeof raw.enabled === 'boolean') status.value = parseCloudStatus(raw);
    else await task.run();
    toast.ok(unsupported.value ? '云中继配置已保存（此固件暂不会连接）' : '云中继配置已保存');
  } catch (e) {
    toast.error(e, '保存云配置失败');
  } finally {
    saving.value = false;
  }
}

async function copy(text: string): Promise<void> {
  if (await copyText(text)) toast.ok('已复制');
  else toast.show('复制失败：浏览器拒绝访问剪贴板', 'error');
}
</script>

<template>
  <div class="stack">
    <MdCard title="中继状态">
      <Skeleton v-if="task.busy.value && !status" :lines="4" />
      <EmptyState v-else-if="!status" tone="error" compact title="读取失败" hint="设备未响应 cloud.status" action-text="重试" @action="task.run()" />
      <template v-else>
        <div class="head">
          <span class="orb" :class="{ on: status.state === 'online' }"><UiIcon :name="meta.icon" :size="26" /></span>
          <div class="head-body">
            <div class="head-title">{{ meta.title }}</div>
            <div class="muted sub mono" :title="status.url || undefined">{{ status.url || '未配置地址' }}</div>
          </div>
          <span class="tag" :class="meta.tone">{{ meta.tag }}</span>
        </div>
        <div v-if="unsupported" class="notice" role="note">
          <UiIcon name="info" :size="18" />
          <div>
            <p><strong>这个固件没有内置云中继客户端</strong>，设备不会连接 Cloud，也就不能从外网远程访问。</p>
            <p>下面的开关和地址仍然会保存在设备上，换成带云中继的固件后自动生效。</p>
          </div>
        </div>
        <p v-if="status.detail" class="muted detail">{{ status.detail }}</p>
        <dl class="kv" style="margin-top: 14px">
          <div><dt>设备 ID</dt><dd class="mono">{{ deviceId ?? '—' }}</dd></div>
          <div v-if="status.claimCode">
            <dt>认领码</dt>
            <dd class="claim">
              <span class="mono code">{{ status.claimCode }}</span>
              <MdButton variant="icon" title="复制认领码" @click="copy(status.claimCode!)"><UiIcon name="content_copy" :size="16" /></MdButton>
            </dd>
          </div>
        </dl>
        <p v-if="status.claimCode" class="muted claim-hint">
          设备尚未被认领。在账号页输入认领码即可绑定到你的账号。
          <RouterLink :to="{ name: 'account' }" class="link">去账号页认领 <UiIcon name="arrow_forward" :size="14" /></RouterLink>
        </p>
        <div class="row" style="justify-content: flex-end; margin-top: 8px">
          <MdButton variant="text" :disabled="task.busy.value" @click="task.run()">{{ task.busy.value ? '刷新中…' : '刷新' }}</MdButton>
        </div>
      </template>
    </MdCard>

    <MdCard title="中继配置">
      <div class="stack">
        <div class="row between">
          <span style="font-size: 14px">启用云中继</span>
          <MdSwitch v-model="enabled" :disabled="!session.isOwner || !status" />
        </div>
        <MdTextField v-model="url" label="Cloud 地址" placeholder="wss://cloud.nyabula.tech" icon="link" :disabled="!session.isOwner || !status" :error="urlError" hint="公共 Cloud 或自建地址，如 ws://192.168.1.10:8080" />
        <div class="row" style="justify-content: flex-end; gap: 10px">
          <span v-if="!session.isOwner" class="muted" style="font-size: 12.5px">仅主人 (owner) 可修改</span>
          <MdButton :disabled="!session.isOwner || !dirty || !!urlError || saving" @click="save">{{ saving ? '保存中…' : '保存' }}</MdButton>
        </div>
      </div>
    </MdCard>
  </div>
</template>

<style scoped>
.head { display: flex; align-items: center; gap: 14px; }
.orb {
  width: 48px;
  height: 48px;
  border-radius: 50%;
  display: grid;
  place-items: center;
  background: var(--md-surface-container-highest);
  color: var(--md-on-surface-variant);
  flex: none;
}
.orb.on { background: rgba(var(--md-primary-rgb), 0.14); color: var(--md-primary); }
.head-body { flex: 1; min-width: 0; }
.head-title { font: 600 16px var(--font-title); color: var(--md-on-surface); }
.sub { font-size: 12.5px; margin-top: 2px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.head .tag { flex: none; }
.notice {
  display: flex;
  align-items: flex-start;
  gap: 10px;
  margin-top: 14px;
  padding: 12px 14px;
  border-radius: var(--radius-m);
  background: color-mix(in srgb, var(--md-tertiary) 14%, var(--md-surface-container));
  color: var(--md-on-surface);
}
.notice > .ui-icon { flex: none; margin-top: 2px; color: var(--md-tertiary); }
.notice p { margin: 0 0 6px; font-size: 13px; line-height: 1.6; }
.notice p:last-child { margin-bottom: 0; }
.detail { font-size: 12.5px; margin: 10px 0 0; line-height: 1.6; word-break: break-word; }
.claim { display: inline-flex; align-items: center; gap: 6px; }
.code { font-size: 16px; letter-spacing: 0.12em; color: var(--md-primary); }
.claim-hint { font-size: 12.5px; margin: 12px 0 0; line-height: 1.6; }
.link { color: var(--md-primary); font-weight: 600; text-decoration: none; display: inline-flex; align-items: center; gap: 2px; margin-left: 4px; }
</style>
