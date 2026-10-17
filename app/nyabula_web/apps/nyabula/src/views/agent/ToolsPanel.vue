<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue';
import { MdButton, MdTextField } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import { useNyabotStore } from '../../stores/nyabot';

interface Tool { name: string; description: string; approval: boolean; example: Record<string, unknown> }
interface Catalog { workspace: string; shellPolicy: number; items: Tool[] }
const session = useSessionStore();
const bot = useNyabotStore();
const catalog = ref<Catalog | null>(null);
const busy = ref(false);
const error = ref('');
const message = ref('');
const keys = ['serp', 'exa', 'tavily', 'news'] as const;
const labels = { serp: 'SerpAPI', exa: 'Exa', tavily: 'Tavily', news: 'NewsAPI' };
const toolLabels: Record<string, string> = { read_file: '读取文件', list_dir: '查看目录', write_file: '写入文件', edit_file: '编辑文件', run_shell: 'Shell 命令', web_search: '网页搜索', news_search: '新闻搜索', get_weather: '查询天气', fetch_url: '读取网页' };
const configured = ref<Record<string, boolean>>({});
const secrets = reactive({ serp: '', exa: '', tavily: '', news: '' });
const clear = reactive({ serp: false, exa: false, tavily: false, news: false });
const available = computed(() => session.connected && session.isOwner && bot.status?.ready);
const dirty = computed(() => keys.some(key => secrets[key] !== '' || clear[key]));
const selected = ref('list_dir');
const input = ref('{}');
const output = ref('');
const reads = computed(() => catalog.value?.items.filter(tool => !tool.approval) ?? []);
watch(selected, () => { input.value = JSON.stringify(reads.value.find(tool => tool.name === selected.value)?.example ?? {}, null, 2); output.value = ''; });
async function load(): Promise<void> {
  if (!available.value || busy.value) return;
  const client = session.client;
  busy.value = true;
  try {
    const [tools, providers] = await Promise.all([session.request('agent.tools.catalog', {}), session.request('agent.tools.providers.get', {})]);
    if (client !== session.client) return;
    catalog.value = tools as unknown as Catalog;
    configured.value = providers as unknown as Record<string, boolean>;
    input.value = JSON.stringify(reads.value.find(tool => tool.name === selected.value)?.example ?? {}, null, 2);
    error.value = '';
  } catch (cause) { error.value = String(cause); }
  finally { busy.value = false; }
}
async function save(): Promise<void> {
  if (!available.value || busy.value) return;
  busy.value = true;
  try {
    const fields = Object.fromEntries(keys.filter(key => secrets[key] || clear[key]).map(key => [key, clear[key] ? '' : secrets[key]]));
    configured.value = await session.request('agent.tools.providers.save', fields) as unknown as Record<string, boolean>;
    for (const key of keys) { secrets[key] = ''; clear[key] = false; }
    message.value = '搜索服务配置已保存。'; error.value = '';
  } catch (cause) { error.value = String(cause); }
  finally { busy.value = false; }
}
async function test(): Promise<void> {
  if (!available.value || busy.value) return;
  busy.value = true; output.value = '';
  try {
    const args: unknown = JSON.parse(input.value);
    if (!args || typeof args !== 'object' || Array.isArray(args)) throw new Error('参数必须是 JSON 对象');
    const result = await session.request('agent.tools.read', { name: selected.value, arguments: args });
    output.value = String(result.text ?? JSON.stringify(result)); error.value = '';
  } catch (cause) { error.value = String(cause); }
  finally { busy.value = false; }
}
watch([() => session.client, available], () => { catalog.value = null; void load(); }, { immediate: true });
</script>

<template>
  <section class="tools-panel">
    <h2>内置工具</h2>
    <p>复用 openvela ai_agent 工具。文件统一保存在工作目录，读操作可直接使用；模型写文件和执行 Shell 时沿用任务审批。</p>
    <p v-if="catalog">工作目录：<code>{{ catalog.workspace }}</code>；Shell：{{ catalog.shellPolicy === 2 ? '当前固件禁用' : catalog.shellPolicy === 0 ? '允许列表模式，命令还需固件支持' : '开发模式' }}。</p>
    <p v-if="error" role="alert" class="error">{{ error }}</p>
    <div class="catalog"><article v-for="tool in catalog?.items" :key="tool.name">
      <strong>{{ toolLabels[tool.name] ?? tool.name }}</strong><code>{{ tool.name }}</code>
      <span>{{ tool.approval ? '模型调用需审批' : '读取工具' }}</span>
      <pre>{{ JSON.stringify(tool.example) }}</pre>
    </article></div>
    <h3>搜索服务</h3>
    <p>网页搜索配置 SerpAPI、Exa 或 Tavily 至少一个；新闻搜索使用 NewsAPI。天气和公开 HTTPS 网页读取无需密钥。网络工具会向相应服务发送查询内容。</p>
    <form @submit.prevent="save">
      <div v-for="key in keys" :key="key" class="provider">
        <MdTextField v-model="secrets[key]" :label="`${labels[key]} 密钥`" type="password" autocomplete="off" :placeholder="configured[key] ? '已设置，留空保留' : '尚未设置'" :disabled="!available || busy || clear[key]" />
        <label><input v-model="clear[key]" type="checkbox" :aria-label="`清除 ${labels[key]} 密钥`" :disabled="!available || busy" />清除已保存密钥</label>
      </div>
      <MdButton :disabled="!available || busy || !dirty">保存搜索服务</MdButton>
      <p role="status">{{ message }}</p>
    </form>
    <details><summary>高级：测试读取工具</summary>
      <p>只运行选择的读取工具，不调用模型。文件路径相对于上方工作目录。</p>
      <label>工具<select v-model="selected" aria-label="测试读取工具" :disabled="busy"><option v-for="tool in reads" :key="tool.name" :value="tool.name">{{ toolLabels[tool.name] }}</option></select></label>
      <label>参数<textarea v-model="input" aria-label="工具参数 JSON" rows="5" :disabled="busy" /></label>
      <MdButton :disabled="!available || busy" @click="test">执行读取测试</MdButton>
      <pre v-if="output" class="output" role="status">{{ output }}</pre>
    </details>
  </section>
</template>

<style scoped>
.tools-panel { max-width: 900px; width: 100%; margin: 0 auto; line-height: 1.6; }
.catalog { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 250px), 1fr)); gap: 12px; }
article { display: flex; flex-direction: column; gap: 4px; padding: 16px; background: var(--md-surface-container); border-radius: 12px; min-width: 0; }
article span { font-size: 13px; color: var(--md-on-surface-variant); }
code, pre { overflow-wrap: anywhere; white-space: pre-wrap; }
form { display: flex; flex-direction: column; gap: 16px; }
.provider { display: flex; flex-direction: column; gap: 8px; }
label:has(input[type="checkbox"]), details > label { display: flex; align-items: center; flex-wrap: wrap; gap: 8px; min-height: 44px; }
select, textarea { padding: 12px; border: 1px solid var(--md-outline); border-radius: 8px; background: var(--md-surface); color: var(--md-on-surface); font: inherit; min-height: 44px; }
textarea { width: 100%; resize: vertical; }
details { margin-top: 28px; }
summary { min-height: 44px; cursor: pointer; }
.output { padding: 16px; background: var(--md-surface-container); }
.error { color: var(--md-error); }
</style>
