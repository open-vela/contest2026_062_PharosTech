<script setup lang="ts">
/* Models: what the compute domain needs under /data/models, what is there,
 * and a way to bring the rest from this browser.
 *
 *   models.list    what is on the device, and how much room is left
 *   PUT /models/upload   resumable upload in 8 MiB pieces (stores/modelUploads)
 *   models.verify  hash a file on the device again, progress via models.status
 *   models.delete  a file, an unfinished upload of it, and their records
 *   compute.status who uses these files: the compute domain, and what it is
 *                  pulling out of /data/models right now (composables/useComputeStatus)
 *
 * The catalogue of expected names is the panel's (lib/deviceModels); the device
 * only knows kinds and file names. The models are third-party files the owner
 * brings: nothing here downloads one. A firmware without models.list ends in
 * "not supported" instead of dead buttons. */
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { EmptyState, MdButton, MdCard, Skeleton, UiIcon, useDialogStore, useToastStore } from '@nyabula/ui';
import { fmtBytes, isUnsupportedError, shortDigest } from '../../../lib/deviceMaint';
import {
  fmtDuration, groupModels, parseModelsList, parseModelsStatus, verifyFailureText,
  type ModelGroup, type ModelKind, type ModelRow, type ModelsStatus,
} from '../../../lib/deviceModels';
import { blobPercent, capabilityLabel, llmStatusText, llmStatusTone, transferText } from '../../../lib/deviceCompute';
import { useComputeStatus } from '../../../composables/useComputeStatus';
import { useModelUploadsStore, type UploadTask } from '../../../stores/modelUploads';
import FileDropZone from '../../../components/FileDropZone.vue';
import { useDeviceTopic } from './deviceTopic';

const VERIFY_POLL_MS = 1500;
/* The device answers slowly while it reads a gigabyte: a few missed polls are not a failure. */
const VERIFY_MISSES = 10;
const DELETE_TIMEOUT_MS = 30000;

const { session, data: list, busy, unsupported, run } = useDeviceTopic('models.list', parseModelsList, '读取模型列表失败');
const uploads = useModelUploadsStore();
const dialog = useDialogStore();
const toast = useToastStore();

const groups = computed<ModelGroup[]>(() => groupModels(list.value));
const usedPercent = computed(() => {
  const l = list.value;
  return l && l.volume > 0 ? Math.min(100, Math.round(((l.volume - l.free) / l.volume) * 100)) : 0;
});
const modelBytes = computed(() => groups.value.reduce((sum, g) => sum + g.bytes, 0));
const refused = ref<string[]>([]);

/* ---- compute domain ---- */

const compute = useComputeStatus();
const computeStatus = computed(() => compute.status.value);
const computeLink = computed(() => {
  const c = computeStatus.value;
  if (!c) return { text: '—', tone: 'info' };
  if (c.linked) return { text: '已连接', tone: 'ok' };
  return c.running ? { text: '未连接 · 正在启动', tone: 'warn' } : { text: '未连接 · 没有运行', tone: 'err' };
});
/** What the compute domain is pulling in, in words; '' when it is idle. */
const computeTransfer = computed(() => {
  const b = computeStatus.value?.blob;
  if (!b || (!b.active && !b.hashing)) return '';
  const name = b.name || '文件';
  return b.active ? transferText(b, `正在把「${name}」搬入计算域`) : `正在核对「${name}」的内容，随后搬入计算域…`;
});

/** Why nothing can be uploaded from this page, or ''. Read at render time:
 *  the stored token is not reactive. */
function blockedText(): string {
  if (uploads.blocked) return uploads.blocked;
  return uploads.token() ? '' : '上传需要密码登录后的会话凭据。请退出后用密码登录，再回到这里。';
}

function onPicks(kind: ModelKind, files: File[]): void {
  refused.value = uploads.add(kind, files, list.value);
}

function dropHint(g: ModelGroup): string {
  const missing = g.rows.filter((r) => r.spec && r.state !== 'present').map((r) => r.name);
  const names = (missing.length ? missing : g.spec.files.map((f) => f.name)).slice(0, 6).join('、');
  return `${missing.length ? '还缺' : '可替换'}：${names}。可一次选多个文件，常见的原始文件名会自动改成这里的名字`;
}

/* ---- upload queue ---- */

function percentOf(t: UploadTask): number {
  if (t.size <= 0) return 0;
  const bytes = t.phase === 'hashing' ? t.hashed : t.phase === 'finishing' && t.deviceHashed > 0 ? t.deviceHashed : t.phase === 'done' ? t.size : t.sent;
  return Math.max(0, Math.min(100, Math.round((bytes / t.size) * 100)));
}

function phaseText(t: UploadTask): string {
  switch (t.phase) {
    case 'queued': return '排队中';
    case 'hashing': return `正在本机计算 SHA-256… ${percentOf(t)}%`;
    case 'checking': return '正在询问设备已收到多少…';
    case 'uploading': {
      const speed = t.rate > 0 ? ` · ${fmtBytes(t.rate)}/s` : '';
      const left = t.eta >= 0 ? ` · 剩余约 ${fmtDuration(t.eta)}` : '';
      return `正在上传 ${fmtBytes(t.sent)} / ${fmtBytes(t.size)}${speed}${left}`;
    }
    case 'finishing': return t.deviceHashed > 0 ? `设备正在校验整个文件… ${percentOf(t)}%` : '已全部送达，设备正在校验整个文件（大模型需要一两分钟）…';
    case 'waiting': return `${t.note || '正在重试'}（已确认 ${fmtBytes(t.confirmed)}，会自动续传）`;
    case 'paused': return `已暂停，设备上保留了 ${fmtBytes(t.confirmed)}`;
    case 'done': return t.skipped ? '设备上已有内容相同的文件，无需再传' : '已上传，两端 SHA-256 一致';
    default: return t.error || '上传失败';
  }
}

const running = (t: UploadTask): boolean => ['hashing', 'checking', 'uploading', 'finishing', 'waiting'].includes(t.phase);

async function cancelTask(t: UploadTask): Promise<void> {
  if (t.confirmed > 0 || t.phase === 'uploading' || t.phase === 'finishing' || t.phase === 'waiting') {
    const ok = await dialog.confirm(`将停止上传「${t.path}」，并删除设备上已收到的部分（${fmtBytes(t.confirmed)}）。想以后接着传，请用「暂停」。`, { title: '取消上传', confirmText: '取消并删除', danger: true });
    if (!ok) return;
  }
  await uploads.cancel(t.id);
}

/* A file that arrived, or a part that was removed: list again. */
watch(() => uploads.changed, () => void run());

/* ---- verify / delete ---- */

const status = ref<ModelsStatus | null>(null);
const acting = ref<string | null>(null);
let verifyTimer: ReturnType<typeof setTimeout> | null = null;
let alive = true;

const verifying = computed(() => (status.value?.verify.state === 'running' ? status.value.verify : null));
function verifyPercent(): number {
  const v = verifying.value;
  return v && v.total > 0 ? Math.min(100, Math.round((v.done / v.total) * 100)) : 0;
}

function pollVerify(path: string, misses: number): void {
  if (verifyTimer) clearTimeout(verifyTimer);
  verifyTimer = setTimeout(async () => {
    verifyTimer = null;
    if (!alive) return;
    let missed = misses;
    try {
      status.value = parseModelsStatus(await session.request('models.status'));
      missed = 0;
    } catch {
      missed++;
    }
    if (!alive) return;
    const v = status.value?.verify;
    if (v?.state === 'running' && v.path === path && missed < VERIFY_MISSES) return pollVerify(path, missed);
    if (v?.path === path && v.state === 'done') {
      toast.ok(v.expected ? `「${path}」校验通过，与上传时记录的 SHA-256 一致` : `「${path}」的 SHA-256 已计算并记录：${shortDigest(v.sha256)}`);
      void run();
    } else if (v?.path === path && v.state === 'failed') {
      toast.show(`「${path}」：${verifyFailureText(v.reason)}`, 'error');
    } else if (missed >= VERIFY_MISSES) {
      toast.warn('设备长时间没有回应，校验结果未知，请稍后刷新');
    }
  }, VERIFY_POLL_MS);
}

async function verify(row: ModelRow): Promise<void> {
  if (!session.isOwner || acting.value || verifying.value) return;
  acting.value = row.path;
  try {
    status.value = parseModelsStatus(await session.request('models.verify', { path: row.path }, { timeoutMs: 15000 }));
    const v = status.value.verify;
    if (v.state === 'failed') toast.show(`「${row.path}」：${verifyFailureText(v.reason)}`, 'error');
    else pollVerify(row.path, 0);
  } catch (e) {
    if (isUnsupportedError(e)) toast.show('此固件不支持校验模型（models.verify）', 'error');
    else if (codeOf(e) === 'EBUSY') toast.warn('设备正在接收或校验另一个文件，请稍后再试');
    else toast.error(e, '开始校验失败');
  } finally {
    acting.value = null;
  }
}

function codeOf(e: unknown): string {
  return typeof e === 'object' && e !== null && typeof (e as { code?: unknown }).code === 'string' ? (e as { code: string }).code : '';
}

async function remove(row: ModelRow): Promise<void> {
  if (!session.isOwner || acting.value) return;
  const what = row.file ? `文件（${fmtBytes(row.file.bytes)}）` : `未传完的部分（${fmtBytes(row.part?.bytes ?? 0)}）`;
  const ok = await dialog.confirm(`将从设备删除「${row.path}」的${what}${row.file && row.part ? '以及未传完的部分' : ''}。${row.spec?.required ? '这是必需文件，删除后对应功能无法使用。' : ''}此操作无法撤销。`, { title: '删除模型文件', confirmText: '删除', danger: true });
  if (!ok) return;
  acting.value = row.path;
  try {
    const next = parseModelsList(await session.request('models.delete', { path: row.path }, { timeoutMs: DELETE_TIMEOUT_MS }));
    list.value = next;
    if (next.removed === false) toast.warn('设备上已经没有这个文件');
    else toast.ok(`已删除「${row.path}」`);
  } catch (e) {
    if (isUnsupportedError(e)) toast.show('此固件不支持删除模型（models.delete）', 'error');
    else if (codeOf(e) === 'EBUSY') toast.warn('设备正在接收或校验文件，请稍后再删');
    else toast.error(e, '删除失败');
  } finally {
    acting.value = null;
  }
}

/** A row the queue is working on shows the queue's state, not the list's. */
function taskFor(row: ModelRow): UploadTask | null {
  return uploads.tasks.find((t) => t.path === row.path && t.phase !== 'done') ?? null;
}

function stateTag(row: ModelRow): { text: string; tone: string } {
  if (row.state === 'present') return { text: '已就绪', tone: 'ok' };
  if (row.state === 'partial') return { text: '未传完', tone: 'warn' };
  return row.spec?.required ? { text: '缺失', tone: 'err' } : { text: '未安装', tone: 'info' };
}

function partText(row: ModelRow): string {
  const p = row.part;
  if (!p) return '';
  return p.total > 0 ? `已收到 ${fmtBytes(p.received)} / ${fmtBytes(p.total)}，重新选择同一个文件即可续传` : `有 ${fmtBytes(p.bytes)} 未完成的数据，缺少续传记录，重新上传会从头开始`;
}

function fmtTime(ms: number): string {
  // Before the clock is set the volume stamps files with 1980: say nothing.
  if (ms < Date.UTC(2020, 0, 1)) return '';
  const d = new Date(ms);
  const two = (n: number): string => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${two(d.getMonth() + 1)}-${two(d.getDate())} ${two(d.getHours())}:${two(d.getMinutes())}`;
}

onBeforeUnmount(() => {
  alive = false;
  if (verifyTimer) clearTimeout(verifyTimer);
});
</script>

<template>
  <div class="stack">
    <MdCard v-if="unsupported">
      <EmptyState compact icon="auto_awesome" title="此固件不支持" hint="设备固件未提供模型管理（models.list），升级固件后可用。" />
    </MdCard>
    <template v-else>
      <MdCard title="模型存储">
        <Skeleton v-if="busy && !list" :lines="3" />
        <EmptyState v-else-if="!list" tone="error" compact title="读取失败" hint="设备未响应 models.list" action-text="重试" @action="run()" />
        <template v-else>
          <div class="space-head">
            <span class="space-icon"><UiIcon name="storage" :size="20" /></span>
            <div class="space-id">
              <div class="space-free">可用 {{ fmtBytes(list.free) }}</div>
              <div class="mono muted space-path">{{ list.root }}</div>
            </div>
            <span class="tag" :class="usedPercent >= 90 ? 'err' : usedPercent >= 75 ? 'warn' : 'ok'">{{ list.volume ? `已用 ${usedPercent}%` : '—' }}</span>
          </div>
          <div class="bar" role="progressbar" aria-valuemin="0" aria-valuemax="100" :aria-valuenow="usedPercent" aria-label="数据分区已用空间">
            <span :style="{ width: usedPercent + '%' }" :class="{ warn: usedPercent >= 75, full: usedPercent >= 90 }" />
          </div>
          <div class="space-nums muted">
            <span>模型占用 {{ fmtBytes(modelBytes) }}</span>
            <span v-if="list.volume">分区共 {{ fmtBytes(list.volume) }}</span>
            <span>单个文件上限 {{ fmtBytes(list.fileLimit) }}</span>
          </div>
          <p class="muted line" style="margin-top: 10px">模型是你自己准备的第三方文件，固件里不带。上传按 8 MB 分片、可断点续传；全部送达后设备会重算整个文件的 SHA-256，与本机一致才会放到最终位置。设备始终保留 {{ fmtBytes(list.margin) }} 给数据库和日志。</p>
          <p v-if="list.truncated" class="line warn-text">文件太多，设备一次没有列全；不常用的文件可以删掉一些。</p>
          <div class="row" style="justify-content: flex-end; margin-top: 8px">
            <MdButton variant="text" :disabled="busy" @click="run()"><UiIcon name="refresh" :size="16" /> {{ busy ? '刷新中…' : '刷新' }}</MdButton>
          </div>
        </template>
      </MdCard>

      <MdCard title="计算域">
        <EmptyState v-if="compute.unsupported.value" compact icon="computer" title="此固件没有计算域"
          hint="这是普通的单系统固件：模型文件可以照常上传和保存，但要刷入带计算域（AMP）的固件，设备才会用它们在 NPU 上运行本机模型。这不是故障。" />
        <Skeleton v-else-if="!computeStatus && !compute.failed.value" :lines="2" />
        <EmptyState v-else-if="!computeStatus" compact icon="computer" title="读取计算域状态失败" hint="设备未响应 compute.status" action-text="重试" @action="compute.refresh()" />
        <template v-else>
          <div class="compute-head">
            <span class="space-icon"><UiIcon :name="computeStatus.linked ? 'link' : 'link_off'" :size="20" /></span>
            <div class="space-id">
              <div class="space-free">计算域 <span class="tag" :class="computeLink.tone">{{ computeLink.text }}</span></div>
              <div class="muted compute-sub">运行模型的 Linux 一侧；上传完成的文件由它按需从这里取走</div>
            </div>
          </div>
          <div class="caps" aria-label="计算域能力">
            <span v-for="c in computeStatus.capabilities" :key="c" class="cap" :title="c">{{ capabilityLabel(c) }}<span class="mono cap-id">{{ c }}</span></span>
            <span v-if="!computeStatus.capabilities.length" class="muted line">{{ computeStatus.linked ? '计算域没有报告任何能力' : '连接后显示能力' }}</span>
          </div>
          <div v-if="computeTransfer" class="compute-transfer" role="status">
            <div class="bar" :class="{ busy: !computeStatus.blob.active || computeStatus.blob.size <= 0 }" role="progressbar" aria-valuemin="0" aria-valuemax="100"
              :aria-valuenow="blobPercent(computeStatus.blob)" aria-label="计算域搬运进度"><span :style="{ width: blobPercent(computeStatus.blob) + '%' }" /></div>
            <p class="line task-text">{{ computeTransfer }}<template v-if="computeStatus.blob.active && computeStatus.blob.size"> · {{ fmtBytes(computeStatus.blob.offset) }} / {{ fmtBytes(computeStatus.blob.size) }}</template></p>
          </div>
          <p v-else class="muted line" style="margin-top: 10px">当前没有文件在搬运。</p>
          <p class="line compute-llm">本机语言模型：<span class="tag" :class="llmStatusTone(computeStatus)">{{ llmStatusText(computeStatus) }}</span>
            <RouterLink :to="{ name: 'agent', params: { key: $route.params.key }, query: { tab: 'config' } }">在 Nyabot「配置」里管理</RouterLink></p>
          <p v-if="computeStatus.lastError" class="line warn-text">最近一次错误：{{ computeStatus.lastError }}</p>
          <p v-if="computeStatus.generationChanges || computeStatus.droppedFrames" class="muted line">本次开机以来计算域重启 {{ computeStatus.generationChanges }} 次 · 丢弃帧 {{ computeStatus.droppedFrames }}</p>
        </template>
      </MdCard>

      <MdCard v-if="uploads.tasks.length" title="上传队列">
        <ul class="tasks">
          <li v-for="t in uploads.tasks" :key="t.id" class="task" :class="t.phase">
            <div class="task-head">
              <div class="task-id">
                <div class="task-path mono" :title="t.path">{{ t.path }}</div>
                <div class="task-sub muted">
                  <span v-if="t.renamed" :title="t.fileName">来自 {{ t.fileName }}</span>
                  <span>{{ fmtBytes(t.size) }}</span>
                  <span v-if="t.sha256" class="mono" :title="`SHA-256 ${t.sha256}`">SHA-256 {{ shortDigest(t.sha256) }}</span>
                </div>
              </div>
              <div class="task-acts">
                <MdButton v-if="running(t) || t.phase === 'queued'" variant="text" @click="uploads.pause(t.id)"><UiIcon name="pause" :size="16" /> 暂停</MdButton>
                <MdButton v-if="t.phase === 'paused'" variant="tonal" @click="uploads.resume(t.id)"><UiIcon name="play_arrow" :size="16" /> 继续</MdButton>
                <MdButton v-if="t.phase === 'failed'" variant="tonal" @click="uploads.resume(t.id)"><UiIcon name="refresh" :size="16" /> 重试</MdButton>
                <MdButton v-if="t.phase !== 'done'" variant="text" @click="cancelTask(t)"><UiIcon name="close" :size="16" /> 取消</MdButton>
                <MdButton v-else variant="text" @click="uploads.dismiss(t.id)"><UiIcon name="check" :size="16" /> 知道了</MdButton>
              </div>
            </div>
            <div v-if="t.phase !== 'done' && t.phase !== 'failed'" class="bar" :class="{ busy: t.phase === 'checking' || (t.phase === 'finishing' && !t.deviceHashed) }" role="progressbar" aria-valuemin="0" aria-valuemax="100" :aria-valuenow="percentOf(t)" :aria-label="`${t.path} 上传进度`">
              <span :style="{ width: percentOf(t) + '%' }" />
            </div>
            <p class="line task-text" :class="{ 'error-text': t.phase === 'failed', 'ok-text': t.phase === 'done' }" role="status">{{ phaseText(t) }}</p>
          </li>
        </ul>
        <div v-if="uploads.tasks.some((t) => t.phase === 'done')" class="row" style="justify-content: flex-end; margin-top: 8px">
          <MdButton variant="text" @click="uploads.clearFinished()">清除已完成</MdButton>
        </div>
      </MdCard>

      <MdCard v-if="refused.length">
        <div class="notice warn">
          <UiIcon name="warning" :size="20" />
          <div>
            <p><strong>有文件没有加入队列：</strong></p>
            <p v-for="line in refused" :key="line" class="detail">{{ line }}</p>
            <MdButton variant="text" @click="refused = []">知道了</MdButton>
          </div>
        </div>
      </MdCard>

      <template v-if="list">
        <MdCard v-for="g in groups" :key="g.spec.kind" class="group">
          <div class="group-head">
            <span class="group-icon"><UiIcon :name="g.spec.icon" :size="20" /></span>
            <h3>{{ g.spec.label }}</h3>
            <span class="tag" :class="g.ready ? 'ok' : g.bytes ? 'warn' : 'info'">{{ g.ready ? '已就绪' : g.bytes ? `还缺 ${g.missingRequired} 个必需文件` : '未安装' }}</span>
            <span v-if="g.bytes" class="muted group-size mono">{{ fmtBytes(g.bytes) }}</span>
          </div>
          <p class="muted line">{{ g.spec.purpose }}</p>
          <p class="muted line source">来源：{{ g.spec.source }}　·　目录：<span class="mono">{{ list.root }}/{{ g.spec.kind }}/</span></p>

          <ul class="rows">
            <li v-for="r in g.rows" :key="r.path" class="file-row">
              <div class="file-id">
                <div class="file-name">
                  <span class="mono" :title="r.path">{{ r.name }}</span>
                  <span v-if="r.spec" class="tag" :class="r.spec.required ? 'info' : ''">{{ r.spec.required ? '必需' : '可选' }}</span>
                  <span v-else class="tag">其他文件</span>
                  <span class="tag" :class="stateTag(r).tone">{{ stateTag(r).text }}</span>
                </div>
                <div class="file-sub muted">
                  <span v-if="r.spec">{{ r.spec.label }}</span>
                  <span v-if="r.file">{{ fmtBytes(r.file.bytes) }}</span>
                  <span v-else-if="r.spec?.typicalBytes">通常约 {{ fmtBytes(r.spec.typicalBytes) }}</span>
                  <span v-if="r.file?.sha256" class="mono" :title="`SHA-256 ${r.file.sha256}`">SHA-256 {{ shortDigest(r.file.sha256) }}</span>
                  <span v-else-if="r.file">SHA-256 未记录，可点「校验」计算</span>
                  <span v-if="r.file && fmtTime(r.file.mtime)">{{ fmtTime(r.file.mtime) }}</span>
                </div>
                <div v-if="taskFor(r)" class="file-sub task-note">{{ phaseText(taskFor(r)!) }}</div>
                <div v-else-if="r.part" class="file-sub warn-text">{{ partText(r) }}</div>
                <div v-if="verifying && verifying.path === r.path" class="file-sub">
                  <div class="bar thin" role="progressbar" aria-valuemin="0" aria-valuemax="100" :aria-valuenow="verifyPercent()" :aria-label="`${r.path} 校验进度`"><span :style="{ width: verifyPercent() + '%' }" /></div>
                  设备正在重新计算 SHA-256… {{ verifyPercent() }}%
                </div>
              </div>
              <div v-if="session.isOwner && (r.file || r.part)" class="file-acts">
                <MdButton v-if="r.file" variant="text" :disabled="!!acting || !!verifying || !!taskFor(r)" @click="verify(r)"><UiIcon name="task_alt" :size="16" /> 校验</MdButton>
                <MdButton variant="text" :disabled="!!acting || !!taskFor(r) || verifying?.path === r.path" @click="remove(r)"><UiIcon name="delete" :size="16" /> 删除</MdButton>
              </div>
            </li>
          </ul>

          <p v-if="blockedText()" class="muted line blocked">{{ blockedText() }}</p>
          <FileDropZone v-else :file="null" multiple :hint="dropHint(g)" :lead="`把${g.spec.label}的文件拖到这里，或点击选择`" @picks="onPicks(g.spec.kind, $event)" />
        </MdCard>
      </template>
    </template>
  </div>
</template>

<style scoped>
.line { font-size: 13px; margin: 0; line-height: 1.6; }
.source { margin-top: 4px; font-size: 12.5px; word-break: break-word; }
.blocked { margin-top: 12px; }
.warn-text { color: var(--md-warning); }
.error-text { color: var(--md-error); }
.ok-text { color: var(--md-on-surface-variant); }
.space-head { display: flex; align-items: center; gap: 12px; min-width: 0; }
.space-icon, .group-icon {
  width: 36px; height: 36px; flex: none; border-radius: var(--radius-m);
  display: grid; place-items: center;
  background: var(--md-secondary-container); color: var(--md-on-secondary-container);
}
.space-id { flex: 1; min-width: 0; }
.space-free { font-size: 15px; font-weight: 600; }
.space-path { font-size: 12px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.space-nums { display: flex; flex-wrap: wrap; gap: 4px 16px; font-size: 12.5px; margin-top: 8px; }
.bar { height: 6px; margin-top: 10px; border-radius: 3px; background: var(--md-outline-variant); overflow: hidden; }
.bar.thin { height: 4px; margin: 4px 0; }
.bar > span { display: block; height: 100%; background: var(--md-primary); transition: width 0.2s linear; }
.bar > span.warn { background: var(--md-warning); }
.bar > span.full { background: var(--md-error); }
.bar.busy > span { width: 100% !important; animation: models-pulse 1.2s ease-in-out infinite; }
@keyframes models-pulse { 0%, 100% { opacity: 0.35; } 50% { opacity: 1; } }

.compute-head { display: flex; align-items: center; gap: 12px; min-width: 0; }
.compute-sub { font-size: 12.5px; line-height: 1.5; }
.caps { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 12px; }
.cap {
  display: inline-flex; align-items: center; gap: 6px; padding: 4px 10px; border-radius: var(--radius-s);
  border: 1px solid var(--md-outline-variant); font-size: 12.5px; font-weight: 600; color: var(--md-on-surface);
}
.cap-id { font-size: 11px; font-weight: 400; color: var(--md-on-surface-variant); }
.compute-llm { margin-top: 10px; display: flex; flex-wrap: wrap; align-items: center; gap: 6px 10px; }
.compute-llm .tag { white-space: normal; overflow-wrap: anywhere; }

.tasks, .rows { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; }
.tasks { gap: 14px; }
.task-head { display: flex; align-items: flex-start; gap: 8px 12px; flex-wrap: wrap; }
.task-id { flex: 1 1 220px; min-width: 0; }
.task-path { font-size: 13.5px; font-weight: 600; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.task-sub, .file-sub { display: flex; flex-wrap: wrap; gap: 2px 12px; font-size: 12.5px; margin-top: 2px; line-height: 1.5; }
.task-sub > span { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 100%; }
.task-acts, .file-acts { display: flex; flex-wrap: wrap; gap: 2px; flex: none; margin-left: auto; }
.task-text { margin-top: 6px; color: var(--md-on-surface-variant); }
.task-note { color: var(--md-primary); }

.group-head { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; margin-bottom: 8px; }
.group-head h3 { margin: 0; font: 600 16px var(--font-title); color: var(--md-on-surface); }
.group-size { margin-left: auto; font-size: 12.5px; }
.rows { margin: 12px 0; border-top: 1px solid var(--md-outline-variant); }
.file-row { display: flex; align-items: flex-start; gap: 8px 12px; flex-wrap: wrap; padding: 10px 0; border-bottom: 1px solid var(--md-outline-variant); }
.file-id { flex: 1 1 240px; min-width: 0; }
.file-name { display: flex; flex-wrap: wrap; align-items: center; gap: 6px; font-size: 13.5px; font-weight: 600; }
.file-name > .mono { overflow-wrap: anywhere; }
.file-sub.task-note, .file-sub.warn-text { display: block; }

.notice { display: flex; align-items: flex-start; gap: 12px; }
.notice > .ui-icon { flex: none; margin-top: 2px; color: var(--md-warning); }
.notice p { margin: 0 0 6px; font-size: 13.5px; line-height: 1.6; color: var(--md-on-surface); }
.notice .detail { font-size: 12.5px; color: var(--md-on-surface-variant); word-break: break-word; }
.md-btn :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
</style>
