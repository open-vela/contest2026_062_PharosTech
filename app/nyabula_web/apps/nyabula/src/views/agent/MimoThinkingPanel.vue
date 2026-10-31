<script setup lang="ts">
import { ref, watch } from 'vue';
import { MdButton } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import { useNyabotStore } from '../../stores/nyabot';

const session = useSessionStore();
const bot = useNyabotStore();
const level = ref('enabled');
const saved = ref('');
const busy = ref(false);
const error = ref('');
const levels = [ ['none', '关闭思考'], ['enabled', '开启思考（默认）'],
  ['minimal', '最低'], ['low', '低'], ['medium', '中'], ['high', '高'],
  ['xhigh', '很高'], ['max', '最大'], ['ultra', '极高'] ];

async function sync(save = false): Promise<void> {
  if (!session.connected || !session.isOwner || !bot.status?.ready || busy.value) return;
  const client = session.client;
  busy.value = true;
  error.value = '';
  try {
    const result = await session.request(`agent.config.thinking.${save ? 'set' : 'get'}`,
      save ? { level: level.value } : {});
    if (client !== session.client) return;
    level.value = saved.value = String(result.level);
  } catch (cause) { error.value = cause instanceof Error ? cause.message : String(cause); }
  finally { busy.value = false; }
}
watch([() => session.client, () => bot.status?.ready], () => {
  saved.value = ''; void sync();
}, { immediate: true });
</script>

<template>
  <section class="thinking">
    <h3>MiMo 思考</h3>
    <p>用于 MiMo 官方接口，下一次模型请求生效，不改变其他服务商。</p>
    <label>思考偏好
      <select v-model="level" :disabled="busy || !saved || !session.isOwner">
        <option v-for="entry in levels" :key="entry[0]" :value="entry[0]">{{ entry[1] }}</option>
      </select>
    </label>
    <p>MiMo 当前仅区分开启与关闭；低、中、高等等级会保存为偏好，但实际思考强度相同。</p>
    <p>也可以说“关闭思考”或“把思考设为高”。</p>
    <div class="actions">
      <MdButton :disabled="busy || !saved || level === saved || !session.isOwner" @click="sync(true)">保存思考设置</MdButton>
      <MdButton variant="text" :disabled="busy || !session.connected" @click="sync()">刷新设备状态</MdButton>
    </div>
    <p v-if="error" role="alert">{{ error }}</p>
    <p v-else-if="saved" role="status">设备当前：{{ saved === 'none' ? '关闭思考' : '开启思考' }}{{ level !== saved ? ' · 有未保存修改' : '' }}</p>
  </section>
</template>

<style scoped>
.thinking { margin-top: 24px; padding-top: 16px; border-top: 1px solid var(--md-outline-variant); }
label { display: flex; flex-direction: column; gap: 8px; }

.actions { display: flex; flex-wrap: wrap; gap: 8px; }
[role=alert] { color: var(--md-error); }
</style>
