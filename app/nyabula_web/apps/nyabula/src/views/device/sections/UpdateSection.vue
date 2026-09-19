<script setup lang="ts">
/* Firmware: update.status readout (running version, A/B slots) and, where the
 * firmware offers it, an update from a local file:
 *
 *   pick -> SHA-256 here -> POST /ota/upload (device hashes too, both must
 *   agree) -> confirm the target slot -> update.apply (device writes the slot
 *   that is not running and verifies it from the media) -> update.reboot ->
 *   after the new slot came up: update.confirm.
 *
 * Nothing is written to a slot before the digests agree and the owner has
 * confirmed. A firmware without this path keeps the old "how updates arrive"
 * note instead of dead buttons. */
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { EmptyState, MdButton, MdCard, Skeleton, UiIcon, useDialogStore, useToastStore } from '@nyabula/ui';
import {
  applyErrorText, fmtBytes, isFirmwareImage, isUnsupportedError, needsConfirm, otaEndpoint, parseUpdateStatus, parseUploadReply,
  pendingReboot, uploadErrorText, type UpdateSlot,
} from '../../../lib/deviceMaint';
import { createSha256 } from '../../../lib/sha256';
import { useDeviceTopic } from './deviceTopic';

const { session, data: status, busy, unsupported, run } = useDeviceTopic('update.status', parseUpdateStatus, '读取固件状态失败');
const dialog = useDialogStore();
const toast = useToastStore();

const CHANNEL_ZH: Record<string, string> = { manual: '手动（OTA 包 / USB）', upload: '网页上传 / USB', stable: '稳定版', beta: '测试版' };
const channel = computed(() => (status.value ? CHANNEL_ZH[status.value.channel] ?? status.value.channel : '—'));
const slotLabel = (name: string): string => (name ? `槽位 ${name.toUpperCase()}` : '未知');

function slotState(s: UpdateSlot): { text: string; tone: string } {
  if (s.running) return { text: '正在运行', tone: 'ok' };
  if (!s.bootable) return { text: '不可启动', tone: 'err' };
  if (s.active) return { text: '下次启动', tone: 'warn' };
  return s.successful ? { text: '备用', tone: 'info' } : { text: '待验证', tone: 'warn' };
}

/* ---- upload ---- */

type Phase = 'idle' | 'hashing' | 'uploading' | 'staged' | 'writing';
const HASH_STEP = 4 * 1024 * 1024;
const POLL_MS = 1500;
/* Writing and reading back a full slot takes a while on an SD card, and the
 * device answers slowly meanwhile: a few missed polls are not a failure. */
const POLL_MISSES = 20;

const phase = ref<Phase>('idle');
const file = ref<File | null>(null);
const digest = ref('');
const progress = ref(0);
const message = ref('');
const acting = ref(false);
const picker = ref<HTMLInputElement | null>(null);
let xhr: XMLHttpRequest | null = null;
let pollTimer: ReturnType<typeof setTimeout> | null = null;
let alive = true;

const endpoint = computed(() => otaEndpoint(session.deviceKey, location));
const target = computed(() => status.value?.target ?? '');
const rebootSlot = computed(() => (status.value ? pendingReboot(status.value) : ''));
const unconfirmed = computed(() => (status.value ? needsConfirm(status.value) : false));
const working = computed(() => phase.value === 'hashing' || phase.value === 'uploading' || phase.value === 'writing');
const percent = computed(() => Math.round(progress.value * 100));

function token(): string | null {
  try {
    return endpoint.value ? localStorage.getItem(endpoint.value.tokenSlot) : null;
  } catch {
    return null;
  }
}

function resetPick(): void {
  file.value = null;
  digest.value = '';
  progress.value = 0;
  if (picker.value) picker.value.value = '';
}

async function onPick(e: Event): Promise<void> {
  const picked = (e.target as HTMLInputElement).files?.[0] ?? null;
  message.value = '';
  digest.value = '';
  progress.value = 0;
  phase.value = 'idle';
  file.value = null;
  if (!picked || !status.value) return;
  if (status.value.maxBytes > 0 && picked.size > status.value.maxBytes) {
    message.value = `文件 ${fmtBytes(picked.size)}，超过槽位容量 ${fmtBytes(status.value.maxBytes)}`;
    return;
  }
  // Same test the device applies; failing here saves the whole transfer.
  const head = new Uint8Array(await picked.slice(0, 64).arrayBuffer());
  if (!isFirmwareImage(head)) {
    message.value = '这不是 NuttX 固件镜像（缺少 arm64 Image 头）。请选择构建产出的 nuttx.bin。';
    return;
  }
  file.value = picked;
}

/** WebCrypto where the page has it; the panel served by the device over plain
 *  http is not a secure context, and there `crypto.subtle` does not exist. */
async function hashFile(f: File): Promise<string> {
  if (window.isSecureContext && globalThis.crypto?.subtle) {
    const out = new Uint8Array(await crypto.subtle.digest('SHA-256', await f.arrayBuffer()));
    return Array.from(out, (b) => b.toString(16).padStart(2, '0')).join('');
  }
  const h = createSha256();
  for (let at = 0; at < f.size; at += HASH_STEP) {
    // Each slice is its own await, so the page keeps painting between them.
    h.update(new Uint8Array(await f.slice(at, at + HASH_STEP).arrayBuffer()));
    progress.value = Math.min(1, (at + HASH_STEP) / f.size);
    if (!alive) throw new Error('cancelled');
  }
  return h.hex();
}

function send(method: 'POST' | 'DELETE', body: File | null, sha: string): Promise<{ status: number; text: string }> {
  return new Promise((resolve) => {
    const ep = endpoint.value;
    const tk = token();
    if (!ep || !tk) return resolve({ status: 401, text: '' });
    const x = new XMLHttpRequest();
    xhr = x;
    x.open(method, ep.url);
    x.setRequestHeader('Authorization', `Bearer ${tk}`);
    if (body) {
      x.setRequestHeader('Content-Type', 'application/octet-stream');
      x.setRequestHeader('X-Nya-Sha256', sha);
      x.upload.onprogress = (ev) => {
        if (ev.lengthComputable && ev.total > 0) progress.value = ev.loaded / ev.total;
      };
    }
    const done = (): void => {
      if (xhr === x) xhr = null;
      resolve({ status: x.status, text: x.responseText ?? '' });
    };
    x.onload = done;
    x.onerror = done;
    x.onabort = done;
    x.send(body);
  });
}

async function upload(): Promise<void> {
  const f = file.value;
  if (!f || working.value) return;
  message.value = '';
  try {
    phase.value = 'hashing';
    progress.value = 0;
    digest.value = await hashFile(f);
    phase.value = 'uploading';
    progress.value = 0;
    const res = await send('POST', f, digest.value);
    if (!alive) return;
    const reply = parseUploadReply(res.status, res.text);
    if (!reply.ok) {
      phase.value = 'idle';
      message.value = uploadErrorText(res.status, reply);
      return;
    }
    if (reply.sha256 !== digest.value || reply.received !== f.size) {
      // The device accepted it against the header, so this cannot happen
      // unless something rewrote the reply; do not apply what is unexplained.
      phase.value = 'idle';
      message.value = '设备回报的校验值与本机不一致，已放弃。';
      void send('DELETE', null, '');
      return;
    }
    phase.value = 'staged';
    await apply();
  } catch (e) {
    phase.value = 'idle';
    if (alive) message.value = `读取文件失败：${e instanceof Error ? e.message : String(e)}`;
  }
}

function cancelUpload(): void {
  xhr?.abort();
}

async function apply(): Promise<void> {
  const f = file.value;
  if (!f || !digest.value || phase.value !== 'staged' || !status.value) return;
  const to = slotLabel(target.value);
  const from = slotLabel(status.value.current.slot);
  const yes = await dialog.confirm(
    `将把 ${f.name}（${fmtBytes(f.size)}）写入${to}。写入并从存储回读校验通过后，${to}会成为下次启动的槽位，设备重启后切换到它。正在运行的${from}不会被改动。\n\n写入需要一两分钟，期间请不要断电。`,
    { title: '写入新固件', confirmText: `写入${to}`, danger: true },
  );
  if (!yes) return;
  try {
    status.value = parseUpdateStatus(await session.request('update.apply', { sha256: digest.value }, { timeoutMs: 15000 }));
    phase.value = 'writing';
    poll(0);
  } catch (e) {
    if (isUnsupportedError(e)) toast.warn('此固件不支持网页写入固件（update.apply）');
    else toast.error(e, '开始写入失败');
  }
}

async function discard(): Promise<void> {
  if (working.value) return;
  await send('DELETE', null, '');
  phase.value = 'idle';
  message.value = '';
  resetPick();
}

function poll(misses: number): void {
  if (pollTimer) clearTimeout(pollTimer);
  pollTimer = setTimeout(async () => {
    pollTimer = null;
    if (!alive) return;
    let missed = misses;
    try {
      status.value = parseUpdateStatus(await session.request('update.status'));
      missed = 0;
    } catch {
      missed++;
    }
    if (!alive) return;
    const state = status.value?.apply.state;
    if (state === 'writing' && missed < POLL_MISSES) return poll(missed);
    phase.value = 'idle';
    if (state === 'done') {
      resetPick();
      toast.ok('新固件已写入并校验通过');
    } else if (state === 'failed' && status.value) {
      message.value = applyErrorText(status.value.apply);
    } else if (state === 'writing') {
      message.value = '设备长时间没有回应，写入结果未知。请稍后刷新查看槽位状态。';
    }
  }, POLL_MS);
}

/* A page opened (or reloaded) while the device is still writing picks the
 * progress up again instead of offering a second upload. */
watch(() => status.value?.apply.state, (state) => {
  if (state === 'writing' && phase.value !== 'writing') {
    phase.value = 'writing';
    poll(0);
  }
}, { immediate: true });

async function reboot(): Promise<void> {
  if (acting.value || !rebootSlot.value) return;
  const to = slotLabel(rebootSlot.value);
  if (!await dialog.confirm(`设备将立即重启并从${to}启动，面板会断开，启动完成后自动重连。`, { title: '重启到新固件', confirmText: '重启' })) return;
  acting.value = true;
  try {
    await session.request('update.reboot');
    toast.ok('设备正在重启，稍后会自动重新连接');
  } catch (e) {
    if (isUnsupportedError(e)) toast.warn('此固件不支持从面板重启（update.reboot）');
    else toast.error(e, '重启失败');
  } finally {
    acting.value = false;
  }
}

async function confirmVersion(): Promise<void> {
  if (acting.value) return;
  acting.value = true;
  try {
    status.value = parseUpdateStatus(await session.request('update.confirm'));
    toast.ok('已确认此版本可用');
  } catch (e) {
    if (isUnsupportedError(e)) toast.warn('此固件不支持确认版本（update.confirm）');
    else toast.error(e, '确认失败');
  } finally {
    acting.value = false;
  }
}

onBeforeUnmount(() => {
  alive = false;
  xhr?.abort();
  if (pollTimer) clearTimeout(pollTimer);
});
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
          <div v-if="unconfirmed" class="notice warn" style="margin-top: 12px">
            <UiIcon name="warning" :size="20" />
            <div>
              <p><strong>{{ slotLabel(status.current.slot) }}还没有被确认可用。</strong>如果设备在这个版本下工作正常，请确认；这只是一条记录，不确认也不会自动回退。</p>
              <MdButton variant="tonal" :disabled="acting || !session.isOwner" @click="confirmVersion()"><UiIcon name="task_alt" :size="16" /> 确认此版本可用</MdButton>
            </div>
          </div>
          <div class="row" style="justify-content: flex-end; margin-top: 12px">
            <MdButton variant="text" :disabled="busy || working" @click="run()"><UiIcon name="refresh" :size="16" /> {{ busy ? '刷新中…' : '刷新' }}</MdButton>
          </div>
        </template>
      </MdCard>

      <MdCard v-if="status" title="A/B 槽位">
        <p v-if="!status.slots.length" class="muted line">设备没有报告 A/B 槽位信息。</p>
        <div v-else class="slots-scroll">
          <table class="slots">
            <thead>
              <tr><th>槽位</th><th>状态</th><th>可启动</th><th>启动已确认</th><th>版本序号</th><th>大小</th></tr>
            </thead>
            <tbody>
              <tr v-for="s in status.slots" :key="s.name" :class="{ on: s.running }">
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
        <p class="muted line" style="margin-top: 12px">新固件总是写入没有在运行的那个槽位，写完从存储回读校验通过才会设为下次启动。引导程序只按优先级选槽位：镜像校验失败时会改用另一个槽位，但固件能启动却工作不正常时不会自动回退。</p>
      </MdCard>

      <MdCard v-if="status && status.upload" title="上传固件">
        <div v-if="rebootSlot && phase === 'idle'" class="notice gap">
          <UiIcon name="check_circle" :size="20" />
          <div>
            <p><strong>{{ slotLabel(rebootSlot) }}已写入新固件，并设为下次启动的槽位。</strong>重启后生效。</p>
            <MdButton :disabled="acting || !session.isOwner" @click="reboot()"><UiIcon name="power" :size="16" /> 重启到新固件</MdButton>
          </div>
        </div>

        <p v-if="!endpoint" class="muted line">固件只能从设备自己提供的页面上传。请在浏览器里直接打开设备地址（http://设备 IP/）再来这里。</p>
        <p v-else-if="!session.isOwner" class="muted line">只有设备主人可以更新固件。</p>
        <p v-else-if="!token()" class="muted line">上传需要密码登录后的会话凭据。请退出后用密码登录，再回到这里。</p>
        <template v-else>
          <p class="muted line">选择构建产出的 NuttX 镜像（nuttx.bin，最大 {{ fmtBytes(status.maxBytes) }}）。文件先传到设备的临时目录，两端 SHA-256 一致后才会询问是否写入{{ target ? slotLabel(target) : '备用槽位' }}。</p>
          <div class="row wrap" style="margin-top: 12px">
            <input ref="picker" class="file" type="file" accept=".bin,.img,application/octet-stream" :disabled="working" @change="onPick" />
          </div>
          <dl v-if="file" class="kv" style="margin-top: 12px">
            <div><dt>文件</dt><dd>{{ file.name }}（{{ fmtBytes(file.size) }}）</dd></div>
            <div v-if="digest"><dt>SHA-256</dt><dd class="mono digest">{{ digest }}</dd></div>
          </dl>
          <div v-if="working" class="progress-block" role="status">
            <div class="bar" :class="{ busy: phase === 'writing' }"><span :style="{ width: (phase === 'writing' ? 100 : percent) + '%' }" /></div>
            <p class="muted line">
              <template v-if="phase === 'hashing'">正在计算 SHA-256… {{ percent }}%</template>
              <template v-else-if="phase === 'uploading'">正在上传… {{ percent }}%</template>
              <template v-else>设备正在写入{{ target ? slotLabel(target) : '槽位' }}并回读校验，请不要断电…</template>
            </p>
          </div>
          <p v-if="message" class="line error-text">{{ message }}</p>
          <div class="row wrap" style="justify-content: flex-end; margin-top: 12px">
            <MdButton v-if="phase === 'uploading'" variant="text" @click="cancelUpload()">取消上传</MdButton>
            <MdButton v-if="phase === 'staged'" variant="text" @click="discard()"><UiIcon name="delete" :size="16" /> 丢弃已上传的文件</MdButton>
            <MdButton v-if="phase === 'staged'" @click="apply()">写入{{ target ? slotLabel(target) : '槽位' }}…</MdButton>
            <MdButton v-if="phase === 'idle' || phase === 'hashing' || phase === 'uploading'" :disabled="!file || working" @click="upload()"><UiIcon name="upload" :size="16" /> 上传并校验</MdButton>
          </div>
        </template>
        <p v-if="status.detail" class="muted line detail mono" style="margin-top: 12px">{{ status.detail }}</p>
      </MdCard>

      <MdCard v-else-if="status" title="如何更新">
        <div class="notice">
          <UiIcon name="info" :size="20" />
          <div>
            <p><strong>此固件不能从面板更新。</strong>设备不会自己检查或下载新版本，也没有网页上传入口，所以这里没有「上传 / 立即升级」按钮。</p>
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
.notice.gap { margin-bottom: 12px; }
.notice > .ui-icon { flex: none; margin-top: 2px; color: var(--md-primary); }
.notice.warn > .ui-icon { color: var(--md-warning); }
.notice p { margin: 0 0 8px; font-size: 13.5px; line-height: 1.65; color: var(--md-on-surface); }
.notice p:last-child { margin-bottom: 0; }
.notice .detail, .detail { font-size: 12.5px; color: var(--md-on-surface-variant); word-break: break-word; }
.file { max-width: 100%; font: inherit; font-size: 13px; color: var(--md-on-surface); }
.digest { font-size: 11.5px; font-weight: 500; }
.progress-block { margin-top: 12px; display: flex; flex-direction: column; gap: 6px; }
.bar { height: 6px; border-radius: 3px; background: var(--md-outline-variant); overflow: hidden; }
.bar > span { display: block; height: 100%; background: var(--md-primary); transition: width 0.2s linear; }
.bar.busy > span { animation: ota-pulse 1.2s ease-in-out infinite; }
@keyframes ota-pulse { 0%, 100% { opacity: 0.35; } 50% { opacity: 1; } }
.error-text { margin-top: 12px; color: var(--md-error); }
.md-btn :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
</style>
