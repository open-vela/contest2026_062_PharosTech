<script setup lang="ts">
import { computed, onMounted, onUnmounted, reactive, ref, watch } from 'vue';
import { MdButton, MdSwitch, MdTextField } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useNasStore } from '../../../stores/nas';
import type { FormFactor } from '../../../composables/useFormFactor';

defineProps<{ type: string; ff: FormFactor }>();
const nas = useNasStore();
const form = reactive({ enabled: false, readOnly: true, device: 'auto', revision: 0 });
const folder = ref('');
const chosen = ref<File | null>(null);
const fileName = ref('');
const saved = ref('');
const dirty = computed(() => saved.value !== JSON.stringify(form));
const parent = computed(() => nas.path.includes('/') ? nas.path.slice(0, nas.path.lastIndexOf('/')) : '');
const statusText = computed(() => !nas.available ? '请连接支持USB文件共享的设备，并以主人身份登录'
  : !nas.status?.enabled ? '共享已关闭'
  : nas.status.ready ? (nas.status.simulated ? '模拟USB盘 · ' : '') + nas.status.mountedDevice + (nas.status.readOnly ? ' · 只读' : ' · 可新增文件')
  : nas.status.lastError === -17 ? '检测到多块设备，请选择要共享的盘'
  : nas.status.lastError === -19 ? '等待USB盘接入'
  : nas.status.lastError ? '挂载失败（' + nas.status.lastError + '），支持 FAT/FAT32/exFAT'
  : '正在挂载');
let poll: ReturnType<typeof setInterval> | undefined;
function load(): void {
  if (!nas.status) return;
  Object.assign(form, { enabled: nas.status.enabled, readOnly: nas.status.readOnly,
    device: nas.status.device, revision: nas.status.revision });
  saved.value = JSON.stringify(form);
}
onMounted(async () => {
  await nas.refresh(true); load();
  poll = setInterval(() => { if (document.visibilityState !== 'hidden') void nas.refresh(); }, 5000);
});
onUnmounted(() => clearInterval(poll));
watch(() => nas.status, () => { if (!saved.value || !dirty.value) load(); });
watch(() => nas.status?.ready, ready => { if (ready) void nas.list(''); });
async function save(): Promise<void> { if (await nas.configure({ ...form })) { load(); if (nas.status?.ready) await nas.list(''); } }
function pick(event: Event): void {
  chosen.value = (event.target as HTMLInputElement).files?.[0] ?? null;
  fileName.value = chosen.value?.name ?? '';
}
function bytes(value?: number): string {
  if (value === undefined) return '—';
  if (value < 1024) return value + ' B';
  if (value < 1024 ** 2) return (value / 1024).toFixed(1) + ' KiB';
  if (value < 1024 ** 3) return (value / 1024 ** 2).toFixed(1) + ' MiB';
  return (value / 1024 ** 3).toFixed(2) + ' GiB';
}
</script>

<template>
  <div class="nas-page">
    <NkHeader icon="storage" title="USB 文件共享" :subtitle="statusText">
      <MdButton variant="tonal" :disabled="!nas.available || nas.busy" @click="nas.refresh(true)">刷新</MdButton>
    </NkHeader>
    <section class="ui-panel card">
      <div class="settings">
        <label><MdSwitch v-model="form.enabled" :disabled="!nas.available" />启用共享</label>
        <label><MdSwitch v-model="form.readOnly" :disabled="!nas.available" />只读共享</label>
        <label>USB盘<select v-model="form.device" :disabled="!nas.available">
          <option value="auto">自动选择唯一USB存储设备</option>
          <option v-if="form.device !== 'auto' && !nas.status?.devices.some(device => device.path === form.device)" :value="form.device">{{ form.device }} · 未接入</option>
          <option v-for="device in nas.status?.devices" :key="device.path" :value="device.path">{{ device.name }} · {{ device.path }}</option>
        </select></label>
      </div>
      <p>只共享选定的USB盘，不访问内部存储，不格式化。关闭只读后可以新增文件和文件夹，始终不覆盖同名文件。</p>
      <p class="muted">通过已登录的设备面板传文件，无匿名访问。请在可信局域网或已有加密隧道中使用；关闭共享后再拔盘。</p>
      <p v-if="nas.status?.simulated" class="muted">当前使用模拟器的独立测试目录，不连接或枚举电脑上的真实USB盘。</p>
      <MdButton :disabled="!nas.available || nas.busy || !dirty || !!nas.status?.activeTransfers" @click="save">保存共享设置</MdButton>
      <p v-if="nas.status?.ready">可用 {{ bytes(nas.status.freeBytes) }} / {{ bytes(nas.status.totalBytes) }} · {{ nas.status.activeTransfers }} 个传输进行中</p>
    </section>
    <p v-if="nas.error" role="alert">{{ nas.error }}</p>
    <p v-if="nas.notice" role="status">{{ nas.notice }}</p>
    <section v-if="nas.status?.ready" class="ui-panel card">
      <div class="row"><h3>文件 · /{{ nas.path }}</h3>
        <MdButton v-if="nas.path" variant="text" :disabled="nas.busy" @click="nas.list(parent)">上一级</MdButton>
      </div>
      <p v-if="nas.transferBlocked" class="muted">{{ nas.transferBlocked }}</p>
      <p v-if="!nas.entries.length && !nas.busy" class="muted">这个文件夹是空的。</p>
      <div class="files">
        <article v-for="entry in nas.entries" :key="entry.path" class="file">
          <div><strong>{{ entry.name }}</strong><small>{{ entry.directory ? '文件夹' : bytes(entry.size) }}</small></div>
          <MdButton v-if="entry.directory" variant="text" :disabled="nas.busy" @click="nas.list(entry.path)">打开</MdButton>
          <MdButton v-else variant="tonal" :disabled="!!nas.transferBlocked" @click="nas.download(entry)">下载</MdButton>
        </article>
      </div>
      <MdButton v-if="nas.more" variant="text" :disabled="nas.busy" @click="nas.list(nas.path, true)">加载更多</MdButton>
    </section>
    <section v-if="nas.status?.ready && !nas.status.readOnly" class="ui-panel card">
      <h3>新增文件</h3>
      <input type="file" aria-label="选择要上传的文件" :disabled="nas.uploading || !!nas.transferBlocked" @change="pick" />
      <MdTextField v-model="fileName" label="上传名称" :disabled="nas.uploading || !chosen" />
      <MdButton :disabled="!chosen || !fileName.trim() || nas.uploading || !!nas.transferBlocked" @click="chosen && nas.upload(chosen, fileName)">上传到当前文件夹</MdButton>
      <div class="row">
        <MdTextField v-model="folder" label="新文件夹名称" />
        <MdButton variant="tonal" :disabled="!folder.trim() || nas.busy" @click="nas.mkdir(folder)">新建文件夹</MdButton>
      </div>
    </section>
    <section v-if="nas.transfer" class="ui-panel card" aria-live="polite">
      <strong>{{ nas.transfer.name }}</strong>
      <progress :value="nas.transfer.sent" :max="nas.transfer.total || 1" />
      <p>{{ bytes(nas.transfer.sent) }} / {{ bytes(nas.transfer.total) }} · {{ nas.uploading ? '上传中；页面内切换不会中断' : nas.transfer.state === 'done' ? '已保存' : nas.transfer.state === 'cancelled' ? '已取消' : '传输失败' }}</p>
      <MdButton v-if="nas.uploading" variant="text" @click="nas.cancel">取消上传</MdButton>
    </section>
  </div>
</template>

<style scoped>
.nas-page,.card { display:flex; flex-direction:column; gap:16px; }
.card { padding:20px; }
.settings,.row { display:flex; flex-wrap:wrap; align-items:center; gap:16px; }
label { display:flex; align-items:center; gap:10px; min-height:44px; }
.settings label:last-child { flex-direction:column; align-items:stretch; min-width:0; max-width:100%; }
select { max-width:100%; }
p,h3 { margin:0; }p { line-height:1.6; }.muted,small { color:var(--md-on-surface-variant); }
.files { display:flex; flex-direction:column; }.file { display:flex; justify-content:space-between; gap:16px; padding:14px 0; border-bottom:1px solid var(--md-outline-variant); }
.file>div { min-width:0; display:flex; flex-direction:column; gap:6px; }.file strong { overflow-wrap:anywhere; }
progress { width:100%; height:10px; }[role=alert] { color:var(--md-error); }
</style>
