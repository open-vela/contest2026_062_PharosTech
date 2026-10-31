<script setup lang="ts">
import { computed, ref } from 'vue';
import { describeError, MdButton } from '@nyabula/ui';
import { usePluginsStore } from '../../stores/plugins';
import { useSessionStore } from '../../stores/session';
import { stateLabel } from './plugins.logic';
const props = defineProps<{ id: string }>();
const plugins = usePluginsStore();
const session = useSessionStore();
const plugin = computed(() => plugins.list.find(p => p.id === props.id));
const busy = ref(false);
const error = ref('');
const version = ref('');
async function run(action: 'start' | 'stop' | 'rollback' | 'activate') {
  busy.value = true; error.value = '';
  try { await plugins.manage(action, props.id, version.value || undefined); }
  catch (e) { error.value = describeError(e); }
  finally { busy.value = false; }
}
</script>
<template>
  <section v-if="plugin" class="lifecycle">
    <div v-if="session.isOwner" class="actions">
      <MdButton :disabled="busy" @click="run(plugin.state === 'running' ? 'stop' : 'start')">{{ plugin.state === 'running' ? '停止插件' : '启动插件' }}</MdButton>
      <MdButton v-if="plugin.versions" variant="text" :disabled="busy || plugin.state === 'running' || !plugin.versions.some(v => v.lastGood)" @click="run('rollback')">回退版本</MdButton>
    </div>
    <div v-if="session.isOwner && (plugin.versions?.length ?? 0) > 1" class="actions">
      <select v-model="version" aria-label="插件版本" :disabled="busy || plugin.state === 'running'">
        <option value="" disabled>选择已安装版本</option>
        <option v-for="v in plugin.versions" :key="v.version" :value="v.version">{{ v.version }}{{ v.current ? '（当前）' : '' }}</option>
      </select>
      <MdButton :disabled="busy || !version || plugin.state === 'running'" @click="run('activate')">切换版本</MdButton>
    </div>
    <p v-if="error" role="alert">{{ error }}</p>
    <p v-if="plugin.lastError" class="muted">运行错误：{{ plugin.lastError }}</p>
  </section>
</template>
<style scoped>
.lifecycle { display: grid; gap: 12px; padding: 14px 0; margin-bottom: 20px; border-bottom: 1px solid var(--md-outline-variant); }
.actions { display: flex; flex-wrap: wrap; gap: 8px; }
p { margin: 0; overflow-wrap: anywhere; }
[role=alert] { color: var(--md-error); }
</style>
