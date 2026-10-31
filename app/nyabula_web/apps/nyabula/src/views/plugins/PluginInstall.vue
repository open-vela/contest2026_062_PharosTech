<script setup lang="ts">
import { computed, ref } from 'vue';
import { describeError, MdButton, UiIcon, useToastStore } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import { usePluginsStore } from '../../stores/plugins';
import { readPluginArchive, uploadPluginArchive, type PluginArchive } from '../../lib/pluginArchive';
const session = useSessionStore(); const plugins = usePluginsStore(); const toast = useToastStore();
const localConnection = computed(() => session.transport !== 'cloud');
const development = computed(() => session.device?.coreVersion?.includes('dev-goja') === true);
const modal = ref<HTMLDialogElement>(); const archive = ref<PluginArchive>(); const filename = ref('');
const path = ref(''); const busy = ref(false); const progress = ref(0); const error = ref('');
async function selectFile(file?: File) {
  error.value = ''; archive.value = undefined; filename.value = file?.name ?? '';
  if (!file) return;
  try { archive.value = readPluginArchive(new Uint8Array(await file.arrayBuffer())); }
  catch (e) { error.value = describeError(e); }
}
function drop(e: DragEvent) { if (!busy.value) void selectFile(e.dataTransfer?.files[0]); }
async function install(fromDirectory = false) {
  if (!localConnection.value) return;
  busy.value = true; error.value = ''; progress.value = 0;
  try {
    if (fromDirectory) await plugins.install(path.value.trim());
    else if (archive.value) {
      await uploadPluginArchive(archive.value, (topic, data) => session.request(topic, data), value => progress.value = value);
      await plugins.refresh();
    } else return;
    modal.value?.close(); archive.value = undefined; filename.value = ''; path.value = '';
    toast.ok('插件已安装，请按需授权后启动');
  } catch (e) {
    const code = e && typeof e === 'object' ? (e as { code?: string }).code : undefined;
    error.value = code === 'EPERM' || code === 'EACCES'
      ? '安装被拒绝：请检查主人权限、插件签名与文件完整性。'
      : describeError(e);
  }
  finally { busy.value = false; }
}
</script>
<template>
  <div v-if="session.isOwner" class="install-entry">
    <MdButton :disabled="!session.connected" @click="modal?.showModal()"><UiIcon name="add" :size="18" /> 安装插件</MdButton>
    <Teleport to="body">
      <dialog ref="modal" class="plugin-install-dialog" aria-labelledby="plugin-install-title" @cancel="busy && $event.preventDefault()">
        <header><div><h2 id="plugin-install-title">安装插件</h2><p>选择 .nya 或 .zip 插件包</p></div><MdButton variant="icon" :disabled="busy" aria-label="关闭安装窗口" @click="modal?.close()"><UiIcon name="close" /></MdButton></header>
        <p v-if="!localConnection" class="install-hint">安装插件需要局域网直连设备，请切换连接方式后再上传。</p>
        <label class="upload-zone" @dragover.prevent @drop.prevent="drop">
          <UiIcon name="upload" :size="32" /><strong>{{ filename || '点击选择，或将插件包拖到这里' }}</strong><span>最大 8 MiB · 最多 128 个文件</span>
          <input type="file" accept=".nya,.zip,application/zip" :disabled="busy || !localConnection" aria-label="选择插件压缩包" @change="selectFile(($event.target as HTMLInputElement).files?.[0])" />
        </label>
        <div v-if="archive" class="package-preview"><strong>{{ archive.name }}</strong><code>{{ archive.id }} · v{{ archive.version }}</code><span>{{ Object.keys(archive.files).length }} 个文件 · {{ (archive.bytes / 1024).toFixed(1) }} KiB</span></div>
        <p class="install-hint">{{ development ? '开发模拟器仅安装你信任的代码；插件保持停止，权限单独授予。' : '设备将校验签名与完整性；安装不会自动授予权限或启动插件。' }}</p>
        <div v-if="busy" role="status"><progress :value="progress" max="100" /><span>{{ progress >= 95 ? '正在验证并安装…' : `正在上传 ${progress}%` }}</span></div>
        <p v-if="error" role="alert">{{ error }}</p>
        <details v-if="!development"><summary>高级：从设备目录安装</summary><div class="directory-install"><input v-model="path" class="field" :disabled="busy" aria-label="设备上的签名插件目录" placeholder="/data/tmp/my-plugin" /><MdButton variant="outlined" :disabled="busy || !path.trim() || !localConnection" @click="install(true)">安装目录</MdButton></div></details>
        <footer><MdButton variant="text" :disabled="busy" @click="modal?.close()">取消</MdButton><MdButton :disabled="busy || !archive || !localConnection" @click="install()">{{ busy ? '安装中…' : '安装插件' }}</MdButton></footer>
      </dialog>
    </Teleport>
  </div>
</template>
<style scoped>
.install-entry{display:flex;justify-content:flex-end;margin-bottom:20px}
.plugin-install-dialog{width:min(540px,calc(100vw - 32px));max-height:calc(100dvh - 40px);overflow:auto;margin:auto;padding:24px;border:1px solid var(--md-outline-variant);border-radius:24px;background:var(--md-surface-container);color:var(--md-on-surface);box-shadow:var(--md-elev-2);font:14px/1.6 var(--font-body)}
.plugin-install-dialog::backdrop{background:#0009;backdrop-filter:blur(6px)}
header{display:flex;justify-content:space-between;align-items:flex-start;gap:20px;margin-bottom:20px}h2{font:600 22px var(--font-title);margin:0}header p{margin:6px 0 0;color:var(--md-on-surface-variant)}
.upload-zone{position:relative;display:flex;flex-direction:column;align-items:center;gap:12px;border:1px dashed var(--md-outline);border-radius:16px;padding:28px 18px;text-align:center;background:var(--md-surface-container-high);cursor:pointer;overflow-wrap:anywhere}.upload-zone:focus-within{outline:2px solid var(--md-primary);outline-offset:3px}.upload-zone>input{position:absolute;inset:0;opacity:0;width:100%;cursor:pointer}.upload-zone>.ui-icon{color:var(--md-primary)}.upload-zone span,.install-hint{font-size:13px;color:var(--md-on-surface-variant)}
.package-preview{display:flex;flex-direction:column;gap:4px;margin-top:18px;overflow-wrap:anywhere}.package-preview strong{font-size:18px}.package-preview code,.package-preview span{font-size:12px;color:var(--md-on-surface-variant)}.install-hint{margin:18px 0}footer{display:flex;justify-content:flex-end;gap:10px;margin-top:24px}progress{display:block;width:100%;accent-color:var(--md-primary)}[role=alert]{color:var(--md-error);overflow-wrap:anywhere}summary{cursor:pointer;color:var(--md-on-surface-variant);font-size:13px}.directory-install{display:flex;flex-wrap:wrap;gap:10px;margin-top:12px}.directory-install input{flex:1;min-width:160px;border:1px solid var(--md-outline);border-radius:8px;background:transparent;color:inherit;padding:10px;font:inherit}
</style>
