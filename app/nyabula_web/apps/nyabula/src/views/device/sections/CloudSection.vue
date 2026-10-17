<script setup lang="ts">
/* Cloud relay: cloud.status readout + cloud.config form (owner only). */
import { computed, ref, watch } from 'vue';
import { EmptyState, MdButton, MdCard, MdSwitch, MdTextField, Skeleton, UiIcon, useToastStore } from '@nyabula/ui';
import { useSessionStore } from '../../../stores/session';
import { useAsyncTask } from '../../../composables/useRequest';

interface CloudStatus {
  enabled?: boolean;
  url?: string;
  connected?: boolean;
  deviceId?: string;
  claimCode?: string;
}

const session = useSessionStore();
const toast = useToastStore();
const task = useAsyncTask<CloudStatus>(() => session.request('cloud.status') as Promise<CloudStatus>, { errorPrefix: '读取云中继状态失败', holdRoute: true, immediate: true });
const status = computed(() => task.data.value);

const enabled = ref(false);
const url = ref('');
watch(status, (s) => {
  if (!s) return;
  enabled.value = s.enabled === true;
  url.value = s.url ?? '';
}, { immediate: true });
const dirty = computed(() => !!status.value && (enabled.value !== (status.value.enabled === true) || url.value.trim() !== (status.value.url ?? '')));
const urlError = computed(() => (url.value.trim() && !/^wss?:\/\/\S+$/.test(url.value.trim()) ? '需以 ws:// 或 wss:// 开头' : null));

const saving = ref(false);
async function save(): Promise<void> {
  if (!session.isOwner || urlError.value) return;
  saving.value = true;
  try {
    await session.request('cloud.config', { enabled: enabled.value, url: url.value.trim() });
    toast.ok('云中继配置已保存');
    await task.run();
  } catch (e) {
    toast.error(e, '保存云配置失败');
  } finally {
    saving.value = false;
  }
}

async function copy(text: string): Promise<void> {
  try {
    await navigator.clipboard.writeText(text);
    toast.ok('已复制');
  } catch (e) {
    toast.error(e, '复制失败');
  }
}
</script>

<template>
  <div class="stack">
    <MdCard title="中继状态">
      <Skeleton v-if="task.busy.value && !status" :lines="4" />
      <EmptyState v-else-if="!status" tone="error" compact title="读取失败" hint="设备未响应 cloud.status" action-text="重试" @action="task.run()" />
      <template v-else>
        <div class="head">
          <span class="orb" :class="{ on: status.connected }"><UiIcon :name="status.connected ? 'cloud' : 'cloud_off'" :size="26" /></span>
          <div class="head-body">
            <div class="head-title">{{ status.connected ? '已连接云端' : status.enabled ? '未连接' : '已停用' }}</div>
            <div class="muted sub mono">{{ status.url || '未配置地址' }}</div>
          </div>
          <span class="tag" :class="status.connected ? 'ok' : status.enabled ? 'warn' : 'info'">{{ status.connected ? '在线' : status.enabled ? '离线' : '停用' }}</span>
        </div>
        <dl class="kv" style="margin-top: 14px">
          <div><dt>设备 ID</dt><dd class="mono">{{ status.deviceId ?? '—' }}</dd></div>
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
.claim { display: inline-flex; align-items: center; gap: 6px; }
.code { font-size: 16px; letter-spacing: 0.12em; color: var(--md-primary); }
.claim-hint { font-size: 12.5px; margin: 12px 0 0; line-height: 1.6; }
.link { color: var(--md-primary); font-weight: 600; text-decoration: none; display: inline-flex; align-items: center; gap: 2px; margin-left: 4px; }
</style>
