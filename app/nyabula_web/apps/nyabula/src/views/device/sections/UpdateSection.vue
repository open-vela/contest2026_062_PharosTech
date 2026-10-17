<script setup lang="ts">
/* Firmware: update.status readout (running version, A/B slots of both boot
 * domains) and, where the firmware offers it, an update from a local file.
 *
 * What can be updated is the device's list (`targets`), never the panel's:
 *   - two ordinary targets, each with its A/B table: the openvela firmware
 *     and the AMP image, staged into the slot of their domain that is not in
 *     use;
 *   - advanced targets (N-Boot itself, single partitions), hidden behind a
 *     switch that first explains what they can do to the device and wants the
 *     device's name typed before it turns on.
 *
 * One flow for all of them:
 *   pick -> SHA-256 here -> POST /ota/upload?target= (device hashes too, both
 *   must agree) -> confirm -> update.apply (device writes and verifies from
 *   the media) -> update.reboot -> for a slot, once it has come up and works:
 *   update.confirm.
 *
 * Nothing is written before the digests agree and the owner has confirmed. A
 * firmware without this path keeps the old "how updates arrive" note instead
 * of dead buttons. */
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { EmptyState, MdButton, MdCard, MdSwitch, Skeleton, UiIcon, useDialogStore, useToastStore } from '@nyabula/ui';
import {
  AMP_TARGET, DEFAULT_TARGET, IMAGE_HEAD_BYTES, appliedTarget, applyErrorText, applyNotice, applyRefusalText, applyRequest, findTarget, fmtBytes,
  formatHint, formatName, imageFormatOk, isMountedRefusal, isUnsupportedError, needsAmpConfirm, needsConfirm, otaEndpoint, otaUploadUrl,
  parseUpdateStatus, parseUploadReply, pendingReboot, shortDigest, unavailableText, unlockMatches, unlockPhrase, uploadErrorText,
  type UpdateTarget,
} from '../../../lib/deviceMaint';
import { createSha256 } from '../../../lib/sha256';
import FileDropZone from '../../../components/FileDropZone.vue';
import SlotTable from './SlotTable.vue';
import { useDeviceTopic } from './deviceTopic';

const { session, data: status, busy, unsupported, run } = useDeviceTopic('update.status', parseUpdateStatus, '读取固件状态失败');
const dialog = useDialogStore();
const toast = useToastStore();

const CHANNEL_ZH: Record<string, string> = { manual: '手动（OTA 包 / USB）', upload: '网页上传 / USB', stable: '稳定版', beta: '测试版' };
const channel = computed(() => (status.value ? CHANNEL_ZH[status.value.channel] ?? status.value.channel : '—'));
const slotLabel = (name: string): string => (name ? `槽位 ${name.toUpperCase()}` : '未知');

/* ---- targets ---- */

const selectedId = ref(DEFAULT_TARGET);
const advancedOn = ref(false);

const standardTargets = computed(() => (status.value?.targets ?? []).filter((t) => !t.advanced));
const advancedTargets = computed(() => (status.value?.targets ?? []).filter((t) => t.advanced));
const selected = computed<UpdateTarget | null>(() => findTarget(status.value, selectedId.value) ?? standardTargets.value[0] ?? null);

function slotsOf(t: UpdateTarget) {
  return t.id === AMP_TARGET ? status.value?.ampSlots ?? [] : status.value?.slots ?? [];
}

/** Where an image for this target ends up, for sentences. */
function destination(t: UpdateTarget): string {
  if (t.kind !== 'slot') return t.label;
  const domain = t.id === AMP_TARGET ? 'AMP ' : '';
  return t.slot ? `${domain}${slotLabel(t.slot)}` : `${domain}备用槽位`;
}

/* ---- upload ---- */

type Phase = 'idle' | 'hashing' | 'uploading' | 'staged' | 'writing';
const HASH_STEP = 4 * 1024 * 1024;
/* WebCrypto wants the whole file in one buffer; past this the page hashes in
 * slices instead, whatever the context. An AMP image is hundreds of MiB. */
const HASH_WHOLE_MAX = 64 * 1024 * 1024;
const POLL_MS = 1500;
/* Writing and reading back a full slot takes a while on an SD card, and the
 * device answers slowly meanwhile: a few missed polls are not a failure. */
const POLL_MISSES = 20;

const phase = ref<Phase>('idle');
const file = ref<File | null>(null);
const digest = ref('');
const progress = ref(0);
const message = ref('');
const forceNotice = ref('');
const acting = ref(false);
let xhr: XMLHttpRequest | null = null;
let pollTimer: ReturnType<typeof setTimeout> | null = null;
let alive = true;
/* Bumped by every pick and removal: a hash that finishes late is dropped. */
let pickSeq = 0;

const endpoint = computed(() => otaEndpoint(session.deviceKey, location));
const rebootSlot = computed(() => (status.value ? pendingReboot(status.value) : ''));
const unconfirmed = computed(() => (status.value ? needsConfirm(status.value) : false));
const ampUnconfirmed = computed(() => (status.value ? needsAmpConfirm(status.value) : false));
const applied = computed(() => (status.value ? appliedTarget(status.value) : null));
const working = computed(() => phase.value === 'hashing' || phase.value === 'uploading' || phase.value === 'writing');
/* A staged file was checked by the device against one target; it stays with it. */
const targetLocked = computed(() => working.value || phase.value === 'staged');
const percent = computed(() => Math.round(progress.value * 100));
const pickHint = computed(() => {
  const t = selected.value;
  return t ? `${formatName(t.format)}，最大 ${fmtBytes(t.maxBytes)}` : '';
});

function token(): string | null {
  try {
    return endpoint.value ? localStorage.getItem(endpoint.value.tokenSlot) : null;
  } catch {
    return null;
  }
}

function resetPick(): void {
  pickSeq++;
  file.value = null;
  digest.value = '';
  progress.value = 0;
}

function select(t: UpdateTarget): void {
  if (targetLocked.value || !t.available || t.id === selectedId.value) return;
  selectedId.value = t.id;
  message.value = '';
  resetPick();
}

/** The warning comes first and the switch only moves once it has been
 *  answered: the advanced targets can leave the device unable to start. */
async function setAdvanced(on: boolean): Promise<void> {
  if (targetLocked.value) return;
  if (!on) {
    advancedOn.value = false;
    const std = standardTargets.value[0];
    if (selected.value?.advanced && std) select(std);
    return;
  }
  if (advancedOn.value) return;
  const phrase = unlockPhrase(session.device?.name);
  const typed = await dialog.prompt(
    '高级选项可以改写引导程序 N-Boot 和单个分区，请先读完：\n\n'
    + '· 写错或写入中断，设备可能再也无法启动。届时只能拆机，经 USB（MaskROM / Loader 模式）用电脑重新刷写，面板帮不上忙。\n'
    + '· N-Boot 的自更新不具备断电安全：它只有一个区域，原位覆盖，没有备份。从开始写入到校验完成，任何断电或复位都会留下一块无法启动的板子。\n'
    + '· 原样写入分区不会更新 A/B 启动记录，设备也无法判断内容是否正确。\n\n'
    + '更新 openvela 固件和 AMP 镜像不需要打开这里。',
    { title: '启用高级选项', confirmText: '我已了解风险，启用', danger: true, input: { label: `请输入「${phrase}」以确认`, placeholder: phrase } },
  );
  if (typed === null) return;
  if (!unlockMatches(typed, phrase)) {
    toast.warn('输入不一致，高级选项保持关闭');
    return;
  }
  advancedOn.value = true;
}

async function onPick(picked: File): Promise<void> {
  const t = selected.value;
  if (working.value || phase.value === 'staged' || !t) return;
  resetPick();
  const seq = pickSeq;
  message.value = '';
  if (t.maxBytes > 0 && picked.size > t.maxBytes) {
    message.value = picked.size > t.capacity
      ? `文件 ${fmtBytes(picked.size)}，超过「${t.label}」的容量 ${fmtBytes(t.capacity)}`
      : `文件 ${fmtBytes(picked.size)}，设备 /data 目前只放得下 ${fmtBytes(t.maxBytes)}`;
    return;
  }
  try {
    // Same test the device applies; failing here saves the whole transfer.
    const head = new Uint8Array(await picked.slice(0, IMAGE_HEAD_BYTES).arrayBuffer());
    if (seq !== pickSeq) return;
    if (!imageFormatOk(t.format, head)) {
      message.value = formatHint(t.format);
      return;
    }
    file.value = picked;
    phase.value = 'hashing';
    const sha = await hashFile(picked, seq);
    if (seq !== pickSeq) return;
    digest.value = sha;
  } catch (e) {
    if (seq !== pickSeq) return;
    file.value = null;
    if (alive) message.value = `读取文件失败：${e instanceof Error ? e.message : String(e)}`;
  } finally {
    if (seq === pickSeq && phase.value === 'hashing') {
      phase.value = 'idle';
      progress.value = 0;
    }
  }
}

function clearPick(): void {
  if (phase.value === 'uploading' || phase.value === 'writing' || phase.value === 'staged') return;
  phase.value = 'idle';
  message.value = '';
  resetPick();
}

/** WebCrypto where the page has it; the panel served by the device over plain
 *  http is not a secure context, and there `crypto.subtle` does not exist. */
async function hashFile(f: File, seq: number): Promise<string> {
  if (window.isSecureContext && globalThis.crypto?.subtle && f.size <= HASH_WHOLE_MAX) {
    const out = new Uint8Array(await crypto.subtle.digest('SHA-256', await f.arrayBuffer()));
    return Array.from(out, (b) => b.toString(16).padStart(2, '0')).join('');
  }
  const h = createSha256();
  for (let at = 0; at < f.size; at += HASH_STEP) {
    // Each slice is its own await, so the page keeps painting between them.
    h.update(new Uint8Array(await f.slice(at, at + HASH_STEP).arrayBuffer()));
    progress.value = Math.min(1, (at + HASH_STEP) / f.size);
    if (!alive || seq !== pickSeq) throw new Error('已取消');
  }
  return h.hex();
}

function send(method: 'POST' | 'DELETE', body: File | null, sha: string, targetId: string): Promise<{ status: number; text: string }> {
  return new Promise((resolve) => {
    const ep = endpoint.value;
    const tk = token();
    if (!ep || !tk) return resolve({ status: 401, text: '' });
    const x = new XMLHttpRequest();
    xhr = x;
    x.open(method, body ? otaUploadUrl(ep.url, targetId) : ep.url);
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
  const t = selected.value;
  if (!f || !t || !digest.value || working.value) return;
  message.value = '';
  phase.value = 'uploading';
  progress.value = 0;
  const res = await send('POST', f, digest.value, t.id);
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
    void send('DELETE', null, '', '');
    return;
  }
  phase.value = 'staged';
  await apply();
}

function cancelUpload(): void {
  xhr?.abort();
}

/** What writing this target means, in the words of the confirm dialog. */
function applyPrompt(t: UpdateTarget, f: File): { text: string; title: string; confirmText: string } {
  const what = `${f.name}（${fmtBytes(f.size)}）`;
  const to = destination(t);
  if (t.kind === 'slot' && t.id === AMP_TARGET) {
    return {
      title: '写入 AMP 镜像', confirmText: `写入${to}`,
      text: `将把 ${what} 写入${to}。写入并从存储回读校验通过后，${to}会被激活：引导程序 N-Boot 下次启动会先尝试这个 AMP 镜像，再回落到 openvela 固件。当前正在使用的 AMP 槽位不会被改动。\n\n镜像较大，写入需要几分钟，期间请不要断电。`,
    };
  }
  if (t.kind === 'slot') {
    const from = slotLabel(status.value?.current.slot ?? '');
    return {
      title: '写入新固件', confirmText: `写入${to}`,
      text: `将把 ${what} 写入${to}。写入并从存储回读校验通过后，${to}会成为下次启动的槽位，设备重启后切换到它。正在运行的${from}不会被改动。\n\n写入需要一两分钟，期间请不要断电。`,
    };
  }
  if (t.kind === 'nboot') {
    return {
      title: '覆盖 N-Boot 引导程序', confirmText: '我确认，覆盖 N-Boot',
      text: `将用 ${what} 原位覆盖引导程序 N-Boot。\n\nN-Boot 只有一个区域，没有备份，这次写入不具备断电安全：从开始到校验完成，断电、复位，或者镜像本身不能用，设备都将无法启动，只能经 USB（MaskROM）用电脑恢复。\n\n请确认电源稳定，并且这个镜像来自官方打包脚本。`,
    };
  }
  return {
    title: `原样写入${t.label}`, confirmText: `我确认，写入${t.label}`,
    text: `将把 ${what} 原样写入${t.label}。\n\n${t.description}\n\n设备只核对校验值并在写入后回读，不判断内容是否正确，也不更新 A/B 启动记录。写错可能导致设备无法启动，只能经 USB（MaskROM）恢复。`,
  };
}

async function apply(): Promise<void> {
  const f = file.value;
  const t = selected.value;
  if (!f || !t || !digest.value || phase.value !== 'staged' || !status.value) return;
  const ask = applyPrompt(t, f);
  if (!await dialog.confirm(ask.text, { title: ask.title, confirmText: ask.confirmText, danger: true })) return;
  await startApply(t, t.mounted && await confirmForce(t));
}

/** Writing under a mounted filesystem is a second, separate yes. Nothing is
 *  unmounted; the device says the same in its answer. */
function confirmForce(t: UpdateTarget): Promise<boolean> {
  return dialog.confirm(
    `${t.label}上的文件系统正在使用中，设备不会卸载它。\n\n强制写入会把新内容直接写到已挂载的文件系统之下：重启之前，这个文件系统读到的内容不可信，它的任何一次写入都可能破坏刚写入的镜像。写入完成后必须立即重启。`,
    { title: '分区正在使用', confirmText: '强制写入', danger: true },
  );
}

async function startApply(t: UpdateTarget, force: boolean): Promise<void> {
  if (t.mounted && !force) return;
  try {
    const raw = await session.request('update.apply', applyRequest(t, digest.value, force), { timeoutMs: 15000 });
    status.value = parseUpdateStatus(raw);
    forceNotice.value = applyNotice(raw);
    phase.value = 'writing';
    poll(0);
  } catch (e) {
    if (isUnsupportedError(e)) {
      toast.warn('此固件不支持网页写入固件（update.apply）');
    } else if (isMountedRefusal(e) && !force) {
      // It was not mounted when the page last asked.
      if (await confirmForce(t)) await startApply({ ...t, mounted: true }, true);
    } else {
      const text = applyRefusalText(e);
      if (text) message.value = text;
      else toast.error(e, '开始写入失败');
    }
  }
}

async function discard(): Promise<void> {
  if (working.value) return;
  await send('DELETE', null, '', '');
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
      toast.ok('已写入并校验通过');
    } else if (state === 'failed' && status.value) {
      message.value = applyErrorText(status.value.apply);
    } else if (state === 'writing') {
      message.value = '设备长时间没有回应，写入结果未知。请稍后刷新查看状态。';
    }
  }, POLL_MS);
}

/* A page opened (or reloaded) while the device is still writing picks the
 * progress up again instead of offering a second upload. */
watch(() => status.value?.apply.state, (state) => {
  if (state === 'writing' && phase.value !== 'writing') {
    const writing = status.value?.apply.target;
    if (writing && findTarget(status.value, writing)) selectedId.value = writing;
    phase.value = 'writing';
    poll(0);
  }
}, { immediate: true });

async function reboot(what: string): Promise<void> {
  if (acting.value) return;
  if (!await dialog.confirm(`设备将立即重启${what}，面板会断开，启动完成后自动重连。`, { title: '重启设备', confirmText: '重启' })) return;
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

async function confirmVersion(target: string): Promise<void> {
  if (acting.value) return;
  acting.value = true;
  try {
    // The default goes without a payload: the request an older firmware knows.
    status.value = parseUpdateStatus(await session.request('update.confirm', target === DEFAULT_TARGET ? undefined : { target }));
    toast.ok(target === AMP_TARGET ? '已确认此 AMP 镜像可用' : '已确认此版本可用');
  } catch (e) {
    if (isUnsupportedError(e)) toast.warn('此固件不支持确认版本（update.confirm）');
    else toast.error(e, '确认失败');
  } finally {
    acting.value = false;
  }
}

onBeforeUnmount(() => {
  alive = false;
  pickSeq++;
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
              <MdButton variant="tonal" :disabled="acting || !session.isOwner" @click="confirmVersion(DEFAULT_TARGET)"><UiIcon name="task_alt" :size="16" /> 确认此版本可用</MdButton>
            </div>
          </div>
          <div class="row" style="justify-content: flex-end; margin-top: 12px">
            <MdButton variant="text" :disabled="busy || working" @click="run()"><UiIcon name="refresh" :size="16" /> {{ busy ? '刷新中…' : '刷新' }}</MdButton>
          </div>
        </template>
      </MdCard>

      <!-- A firmware that takes no upload: the table and how updates arrive. -->
      <template v-if="status && !status.upload">
        <MdCard title="A/B 槽位">
          <SlotTable :slots="status.slots" />
        </MdCard>
        <MdCard title="如何更新">
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

      <template v-else-if="status">
        <!-- The two ordinary targets, each with the A/B table of its domain. -->
        <MdCard v-for="t in standardTargets" :key="t.id" class="target" :class="{ selected: selected?.id === t.id }">
          <div class="target-head">
            <div class="target-title">
              <UiIcon :name="t.id === AMP_TARGET ? 'widgets' : 'download'" :size="20" />
              <h3>{{ t.label }}</h3>
              <span v-if="selected?.id === t.id" class="tag ok">更新此项</span>
            </div>
            <MdButton v-if="selected?.id !== t.id" variant="tonal" :disabled="targetLocked || !t.available" @click="select(t)">选择此项</MdButton>
          </div>
          <p v-if="t.description" class="muted line">{{ t.description }}</p>
          <p v-if="!t.available" class="line error-text">{{ unavailableText(t) }}</p>
          <div class="target-table"><SlotTable :slots="slotsOf(t)" /></div>

          <template v-if="t.id === AMP_TARGET">
            <p class="muted line" style="margin-top: 12px">
              <template v-if="status.ampActive">AMP 槽位处于激活状态：引导程序下次启动会先尝试 AMP 镜像，失败才回落到 openvela 固件。</template>
              <template v-else>AMP 槽位当前没有可启动的镜像，设备只启动 openvela 固件。</template>
              新镜像写入非活动槽位并在校验通过后激活，重启后生效。
            </p>
            <div v-if="ampUnconfirmed" class="notice warn" style="margin-top: 12px">
              <UiIcon name="warning" :size="20" />
              <div>
                <p><strong>激活的 AMP 镜像还没有被确认可用。</strong>请在观察到 AMP 计算域工作正常之后再确认；这是你的判断，设备不会替你确认。</p>
                <MdButton variant="tonal" :disabled="acting || !session.isOwner" @click="confirmVersion(AMP_TARGET)"><UiIcon name="task_alt" :size="16" /> 确认此 AMP 镜像可用</MdButton>
              </div>
            </div>
          </template>
          <p v-else class="muted line" style="margin-top: 12px">新固件总是写入没有在运行的那个槽位，写完从存储回读校验通过才会设为下次启动。引导程序只按优先级选槽位：镜像校验失败时会改用另一个槽位，但固件能启动却工作不正常时不会自动回退。</p>
        </MdCard>

        <MdCard v-if="advancedTargets.length" title="高级选项">
          <div class="row between wrap">
            <p class="muted line adv-lead">更新引导程序 N-Boot 或原样写入单个分区。可能导致设备无法启动，平时不需要打开。</p>
            <MdSwitch :model-value="advancedOn" :disabled="targetLocked || !session.isOwner" label="显示高级目标" @update:model-value="setAdvanced($event)" />
          </div>
          <template v-if="advancedOn">
            <div class="notice warn" style="margin-top: 12px">
              <UiIcon name="warning" :size="20" />
              <div><p>这些目标写错或写入中断，设备可能无法启动，只能经 USB（MaskROM）恢复。N-Boot 自更新不具备断电安全。</p></div>
            </div>
            <ul class="adv-list" role="radiogroup" aria-label="高级更新目标">
              <li v-for="t in advancedTargets" :key="t.id">
                <!-- A div, not a button: with the row as a <button> the text column
                     rendered empty in Chromium on the device page (only the radio
                     dot showed), whatever the styles said. -->
                <div
                  class="adv-row" role="radio" :aria-checked="selected?.id === t.id"
                  :aria-disabled="targetLocked || !t.available" :tabindex="targetLocked || !t.available ? -1 : 0"
                  :class="{ on: selected?.id === t.id, off: targetLocked || !t.available }"
                  @click="!(targetLocked || !t.available) && select(t)"
                  @keydown.enter.prevent="!(targetLocked || !t.available) && select(t)"
                  @keydown.space.prevent="!(targetLocked || !t.available) && select(t)"
                >
                  <span class="radio" aria-hidden="true" />
                  <span class="adv-text">
                    <span class="adv-name">{{ t.label }}
                      <span class="tag" :class="t.kind === 'nboot' ? 'err' : 'warn'">{{ t.kind === 'nboot' ? '引导程序' : '原样写入' }}</span>
                      <span v-if="t.mounted" class="tag warn">使用中</span>
                      <span v-if="!t.available" class="tag info">不可写</span>
                    </span>
                    <span class="adv-desc">{{ t.description }}</span>
                    <span class="adv-desc">{{ t.available ? `${formatName(t.format)}，分区 ${fmtBytes(t.capacity)}` : unavailableText(t) }}</span>
                  </span>
                </div>
              </li>
            </ul>
          </template>
        </MdCard>

        <MdCard title="上传与写入">
          <div v-if="rebootSlot && phase === 'idle'" class="notice gap">
            <UiIcon name="check_circle" :size="20" />
            <div>
              <p><strong>{{ slotLabel(rebootSlot) }}已写入新固件，并设为下次启动的槽位。</strong>重启后生效。</p>
              <MdButton :disabled="acting || !session.isOwner" @click="reboot(`并从${slotLabel(rebootSlot)}启动`)"><UiIcon name="power" :size="16" /> 重启到新固件</MdButton>
            </div>
          </div>
          <div v-else-if="applied && phase === 'idle'" class="notice gap" :class="{ warn: status.apply.forced }">
            <UiIcon :name="status.apply.forced ? 'warning' : 'check_circle'" :size="20" />
            <div>
              <p><strong>「{{ applied.label }}」已写入并校验通过。</strong>重启后生效。</p>
              <p v-if="status.apply.forced">{{ forceNotice || '这次写入是在文件系统仍然挂载的情况下完成的，请立即重启，重启前不要再改动设备上的设置。' }}</p>
              <MdButton :disabled="acting || !session.isOwner" @click="reboot('')"><UiIcon name="power" :size="16" /> 立即重启</MdButton>
            </div>
          </div>

          <p v-if="!endpoint" class="muted line">固件只能从设备自己提供的页面上传。请在浏览器里直接打开设备地址（http://设备 IP/）再来这里。</p>
          <p v-else-if="!session.isOwner" class="muted line">只有设备主人可以更新固件。</p>
          <p v-else-if="!token()" class="muted line">上传需要密码登录后的会话凭据。请退出后用密码登录，再回到这里。</p>
          <p v-else-if="!selected" class="muted line">设备没有报告可以更新的目标。</p>
          <template v-else>
            <dl class="kv target-kv">
              <div><dt>更新目标</dt><dd>{{ selected.label }}<span v-if="selected.advanced" class="tag warn adv-tag">高级</span></dd></div>
              <div><dt>写入位置</dt><dd>{{ destination(selected) }}</dd></div>
            </dl>
            <p class="muted line" style="margin: 10px 0 12px">文件先传到设备的临时目录，两端 SHA-256 一致后才会询问是否写入；写入后设备从存储回读再校验一次。</p>
            <FileDropZone
              :file="file" :size-text="file ? fmtBytes(file.size) : ''" :digest="digest" :digest-short="shortDigest(digest)" :hint="pickHint"
              accept=".bin,.img,.itb,.fit,application/octet-stream" :disabled="!selected.available" :locked="phase !== 'idle'"
              @pick="onPick" @clear="clearPick()"
            />
            <div v-if="working" class="progress-block" role="status">
              <div class="bar" :class="{ busy: phase === 'writing' }"><span :style="{ width: (phase === 'writing' ? 100 : percent) + '%' }" /></div>
              <p class="muted line">
                <template v-if="phase === 'hashing'">正在计算 SHA-256… {{ percent }}%</template>
                <template v-else-if="phase === 'uploading'">正在上传… {{ percent }}%</template>
                <template v-else>设备正在写入{{ destination(selected) }}并回读校验，请不要断电…</template>
              </p>
            </div>
            <p v-if="message" class="line error-text">{{ message }}</p>
            <div class="row wrap" style="justify-content: flex-end; margin-top: 12px">
              <MdButton v-if="phase === 'hashing'" variant="text" @click="clearPick()">取消</MdButton>
              <MdButton v-if="phase === 'uploading'" variant="text" @click="cancelUpload()">取消上传</MdButton>
              <MdButton v-if="phase === 'staged'" variant="text" @click="discard()"><UiIcon name="delete" :size="16" /> 丢弃已上传的文件</MdButton>
              <MdButton v-if="phase === 'staged'" @click="apply()">写入{{ destination(selected) }}…</MdButton>
              <MdButton v-if="phase === 'idle' || phase === 'hashing' || phase === 'uploading'" :disabled="!file || !digest || working" @click="upload()"><UiIcon name="upload" :size="16" /> 上传并校验</MdButton>
            </div>
          </template>
          <p v-if="status.detail" class="muted line detail mono" style="margin-top: 12px">{{ status.detail }}</p>
        </MdCard>
      </template>
    </template>
  </div>
</template>

<style scoped>
.line { font-size: 13px; margin: 0; line-height: 1.6; }
.notice { display: flex; align-items: flex-start; gap: 12px; color: var(--md-on-surface-variant); }
.notice.gap { margin-bottom: 12px; }
.notice > .ui-icon { flex: none; margin-top: 2px; color: var(--md-primary); }
.notice.warn > .ui-icon { color: var(--md-warning); }
.notice p { margin: 0 0 8px; font-size: 13.5px; line-height: 1.65; color: var(--md-on-surface); }
.notice p:last-child { margin-bottom: 0; }
/* A notice with an action: the sentence and its button share a row; when the
   button has to wrap it sits at the right edge, under the end of the text. */
.notice > div { flex: 1 1 auto; min-width: 0; display: flex; flex-wrap: wrap; align-items: center; gap: 8px 16px; }
.notice > div > p { flex: 1 1 320px; margin: 0; }
.notice > div > :not(p) { flex: none; margin-left: auto; }
.notice .detail, .detail { font-size: 12.5px; color: var(--md-on-surface-variant); word-break: break-word; }
.target { border: 1.5px solid transparent; transition: border-color var(--dur-fast); }
.target.selected { border-color: var(--md-primary); }
.target-head { display: flex; align-items: center; justify-content: space-between; gap: 12px; flex-wrap: wrap; margin-bottom: 8px; min-height: 40px; }
.target-title { display: flex; align-items: center; gap: 8px; min-width: 0; }
.target-title > .ui-icon { color: var(--md-primary); flex: none; }
.target-title h3 { margin: 0; font: 600 16px var(--font-title); color: var(--md-on-surface); }
.target-table { margin-top: 12px; }
.target-kv { margin-bottom: 0; }
.adv-tag { margin-left: 8px; }
.adv-lead { flex: 1 1 260px; }
.adv-list { list-style: none; margin: 12px 0 0; padding: 0; display: flex; flex-direction: column; gap: 6px; }
.adv-row {
  width: 100%;
  display: flex;
  align-items: flex-start;
  gap: 12px;
  padding: 10px 12px;
  text-align: left;
  font: inherit;
  color: var(--md-on-surface);
  background: var(--md-surface-container-high);
  border: 1.5px solid transparent;
  border-radius: var(--radius-m);
  cursor: pointer;
  transition: border-color var(--dur-fast), background var(--dur-fast);
}
.adv-row.off { cursor: default; opacity: 0.55; }
.adv-row.on { border-color: var(--md-primary); }
.adv-row:focus-visible { outline: 2px solid var(--md-primary); outline-offset: 2px; }
.radio { flex: none; width: 18px; height: 18px; margin-top: 2px; border-radius: 50%; border: 2px solid var(--md-outline); display: grid; place-items: center; }
.adv-row.on .radio { border-color: var(--md-primary); }
.adv-row.on .radio::after { content: ''; width: 8px; height: 8px; border-radius: 50%; background: var(--md-primary); }
.adv-text { flex: 1 1 auto; display: flex; flex-direction: column; gap: 2px; min-width: 0; }
.adv-name { font-size: 14px; font-weight: 600; display: flex; flex-wrap: wrap; align-items: center; gap: 6px; }
.adv-desc { font-size: 12.5px; line-height: 1.55; color: var(--md-on-surface-variant); }
.progress-block { margin-top: 12px; display: flex; flex-direction: column; gap: 6px; }
.bar { height: 6px; border-radius: 3px; background: var(--md-outline-variant); overflow: hidden; }
.bar > span { display: block; height: 100%; background: var(--md-primary); transition: width 0.2s linear; }
.bar.busy > span { animation: ota-pulse 1.2s ease-in-out infinite; }
@keyframes ota-pulse { 0%, 100% { opacity: 0.35; } 50% { opacity: 1; } }
.error-text { margin-top: 12px; color: var(--md-error); }
.md-btn :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
</style>
