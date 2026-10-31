import { computed, ref, watch } from 'vue';
import { defineStore } from 'pinia';
import { useSessionStore } from './session';
import { modelsEndpoint } from '../lib/deviceModels';

export interface NasStatus {
  enabled: boolean; mounted: boolean; ready: boolean; readOnly: boolean;
  device: string; mountedDevice: string; revision: number; activeTransfers: number;
  lastError: number; totalBytes?: number; freeBytes?: number;
  simulated?: boolean;
  devices: { name: string; path: string }[];
}
export interface NasEntry { name: string; path: string; directory: boolean; size: number; modifiedAt: number }

export const useNasStore = defineStore('nas', () => {
  const session = useSessionStore();
  const status = ref<NasStatus | null>(null);
  const path = ref('');
  const entries = ref<NasEntry[]>([]);
  const more = ref(false);
  const offset = ref(0);
  const busy = ref(false);
  const error = ref('');
  const notice = ref('');
  const transfer = ref<{ name: string; sent: number; total: number; state: 'uploading' | 'done' | 'failed' | 'cancelled' } | null>(null);
  let generation = 0;
  let request: XMLHttpRequest | null = null;
  const available = computed(() => session.connected && session.isOwner &&
    session.client?.capabilities.includes('core.nas-v1') === true);
  const endpoint = computed(() => modelsEndpoint(session.deviceKey, location));
  const uploading = computed(() => transfer.value?.state === 'uploading');
  const transferBlocked = computed(() => !endpoint.value
    ? '文件传输请直接打开设备提供的面板；远程连接仍可管理共享状态。'
    : !session.isOwner ? '只有主人可以传输USB盘文件。' : '');

  function describe(cause: unknown): string {
    return cause instanceof Error ? cause.message : String(cause);
  }
  async function refresh(files = false): Promise<void> {
    if (!available.value) return;
    const current = generation;
    try {
      const result = await session.request('nas.status');
      if (current !== generation) return;
      status.value = result as unknown as NasStatus;
      if (files && status.value.ready) await list(path.value);
      if (!status.value.ready) { entries.value = []; more.value = false; }
    } catch (cause) { if (current === generation) error.value = describe(cause); }
  }
  async function list(next: string, append = false): Promise<void> {
    if (!available.value || busy.value) return;
    const current = generation;
    busy.value = true; error.value = '';
    try {
      const result = await session.request('nas.list', { path: next, offset: append ? offset.value : 0 });
      if (current !== generation) return;
      path.value = next;
      const items = Array.isArray(result.entries) ? result.entries as NasEntry[] : [];
      entries.value = append ? [...entries.value, ...items] : items;
      more.value = result.more === true;
      offset.value = Number(result.nextOffset ?? 0);
    } catch (cause) { if (current === generation) error.value = describe(cause); }
    finally { if (current === generation) busy.value = false; }
  }
  async function configure(data: Record<string, unknown>): Promise<boolean> {
    if (!available.value || busy.value) return false;
    const current = generation;
    busy.value = true; error.value = '';
    try {
      const result = await session.request('nas.configure', data);
      if (current !== generation) return false;
      status.value = result as unknown as NasStatus;
      path.value = ''; entries.value = []; more.value = false;
      return true;
    } catch (cause) { if (current === generation) error.value = describe(cause); return false; }
    finally { if (current === generation) busy.value = false; }
  }
  async function mkdir(name: string): Promise<void> {
    if (!name.trim() || name.includes('/') || name.includes('\\')) { error.value = '请输入单个文件夹名称。'; return; }
    const current = generation;
    try {
      await session.request('nas.mkdir', { path: path.value ? path.value + '/' + name.trim() : name.trim() });
      if (current === generation) await list(path.value);
    } catch (cause) { if (current === generation) error.value = describe(cause); }
  }
  async function download(entry: NasEntry): Promise<void> {
    if (!endpoint.value || !available.value || entry.directory) return;
    const current = generation;
    error.value = '';
    try {
      const result = await session.request('nas.download', { path: entry.path });
      if (current !== generation || !endpoint.value) return;
      const url = new URL('/nas/file', endpoint.value.url);
      url.searchParams.set('path', entry.path);
      url.searchParams.set('ticket', String(result.ticket));
      const anchor = document.createElement('a');
      anchor.href = url.toString(); anchor.download = entry.name; anchor.rel = 'noreferrer';
      document.body.append(anchor); anchor.click(); anchor.remove();
      notice.value = '已交给浏览器下载，请在浏览器下载列表查看进度。';
    } catch (cause) { if (current === generation) error.value = describe(cause); }
  }
  function upload(file: File, name: string): void {
    if (!available.value || !endpoint.value || uploading.value || status.value?.readOnly) return;
    if (!name.trim() || name.includes('/') || name.includes('\\')) { error.value = '请输入单个文件名。'; return; }
    const token = localStorage.getItem(endpoint.value.tokenSlot);
    if (!token) { error.value = '请先登录设备面板。'; return; }
    const current = generation;
    const destination = path.value ? path.value + '/' + name.trim() : name.trim();
    const url = new URL('/nas/file', endpoint.value.url);
    url.searchParams.set('path', destination);
    const xhr = new XMLHttpRequest();
    request = xhr; error.value = ''; notice.value = '';
    transfer.value = { name: destination, sent: 0, total: file.size, state: 'uploading' };
    xhr.open('PUT', url.toString());
    xhr.setRequestHeader('Authorization', 'Bearer ' + token);
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.timeout = 30 * 60 * 1000;
    xhr.upload.onprogress = event => {
      if (current === generation && transfer.value) transfer.value.sent = event.loaded;
    };
    xhr.onload = () => {
      if (current !== generation || !transfer.value) return;
      if (xhr.status === 201) {
        transfer.value.sent = file.size; transfer.value.state = 'done';
        notice.value = '上传已保存到USB盘。'; void refresh(true);
      } else {
        transfer.value.state = 'failed';
        error.value = xhr.status === 409 ? '同名文件已存在，未覆盖。修改上传名称后重试。'
          : xhr.status === 403 ? '当前共享只读，或登录已失效。'
          : '上传未完成（HTTP ' + xhr.status + '），目标文件未发布。';
      }
      request = null;
    };
    xhr.onerror = xhr.ontimeout = () => {
      if (current !== generation || !transfer.value) return;
      transfer.value.state = 'failed'; error.value = '传输连接中断，目标文件未发布。重连后可重试，不会覆盖已有文件。'; request = null;
    };
    xhr.onabort = () => {
      if (current !== generation || !transfer.value) return;
      transfer.value.state = 'cancelled'; notice.value = '已取消上传。'; request = null;
    };
    xhr.send(file);
  }
  function cancel(): void { request?.abort(); }
  watch(() => session.deviceKey, () => {
    generation++; request?.abort(); request = null;
    status.value = null; path.value = ''; entries.value = []; transfer.value = null;
    more.value = false; offset.value = 0; busy.value = false; error.value = ''; notice.value = '';
  });
  return { status, path, entries, more, busy, error, notice, transfer, available,
    uploading, transferBlocked, refresh, list, configure, mkdir, download, upload, cancel };
});
