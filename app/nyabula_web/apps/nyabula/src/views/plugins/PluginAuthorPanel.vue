<script setup lang="ts">
import { ref, watch } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { MdButton, MdSwitch } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import { useNyabotStore } from '../../stores/nyabot';

const session = useSessionStore();
const bot = useNyabotStore();
const route = useRoute();
const router = useRouter();
const enabled = ref(false);
const nas = ref(false);
const nasSupported = ref(false);
const loaded = ref(false);
const busy = ref(false);
const error = ref('');
let generation = 0;
const examples = [
  { title: '本地记账本', prompt: '帮我生成并安装一个本地记账插件：记录金额、分类和备注，统计本月总额及分类合计，显示最近10笔，支持删除输错的单条记录。输入必须校验，数据跨重启保留。用中文界面，实际测试新增、统计与删除，再告诉我如何打开。' },
  { title: '传输时间计算器', prompt: '生成并安装一个实用的文件传输时间计算器：区分GB/GiB、MB/s/Mbps，输入文件容量、传输速率和效率百分比，计算预计耗时及实际吞吐；保留最近5次计算。校验零值和负数，用已知算例验证换算，不要做点击计数器。' },
  { title: '设备体检', prompt: '生成并安装一个设备体检插件：读取真实音频、蓝牙与USB状态，展示连接、音量、静音和外接设备；可以刷新并保留最近一次检查摘要。不要伪造设备，不修改硬件设置。先检查实际接口，测试界面和刷新。' },
];

async function sync(value?: boolean, nasValue?: boolean): Promise<void> {
  if (!session.connected || !session.isOwner || busy.value) return;
  const current = generation;
  busy.value = true; error.value = '';
  try {
    const result = await session.request(
      'plugin.author.policy.' + (value === undefined ? 'get' : 'set'),
      value === undefined ? {} : { enabled: value, ...(nasValue === undefined ? {} : { nas: nasValue }) });
    if (current !== generation) return;
    enabled.value = result.enabled === true;
    nas.value = result.nas === true;
    nasSupported.value = result.nasSupported === true;
    loaded.value = true;
  } catch (cause) {
    if (current === generation) error.value = cause instanceof Error ? cause.message : String(cause);
  } finally { if (current === generation) busy.value = false; }
}
function compose(prompt: string): void {
  bot.newConversation();
  bot.draft = prompt;
  void router.push({ name: 'agent', params: { key: route.params.key }, query: { tab: 'chat' } });
}
function setNas(value: boolean): void { void sync(enabled.value, value); }
watch(() => [session.client, session.connected, session.isOwner], () => {
  generation++; busy.value = false; loaded.value = false; error.value = '';
  void sync();
}, { immediate: true });
</script>

<template>
  <section v-if="session.isOwner" class="ui-panel author-panel">
    <div class="author-heading">
      <div><h3>让猫猫自己做工具</h3><p>用自然语言提出需求，设备上的 Agent 生成、安装并测试插件；继续提要求即可迭代。</p></div>
      <label><MdSwitch :model-value="enabled" :disabled="busy || !loaded" @update:model-value="sync" />自动生成与安装</label>
    </div>
    <p class="scope">自动权限限于插件私有存储、界面、猫眼提示、公开天气查询及设备只读状态。启动失败恢复上一版代码与私有数据。</p>
    <label v-if="nasSupported"><MdSwitch :model-value="nas" :disabled="busy || !enabled || !loaded" @update:model-value="setNas" />允许生成USB共享控制插件</label>
    <p v-if="nasSupported && nas" class="scope">这类插件可浏览USB目录、新建文件夹、启停共享及切换读写；不能格式化或覆盖同名文件。文件传输仍走已登录的USB文件共享页。</p>
    <div class="examples">
      <MdButton v-for="example in examples" :key="example.title" variant="tonal" :disabled="!enabled || busy || !loaded" @click="compose(example.prompt)">{{ example.title }}</MdButton>
    </div>
    <p v-if="error" role="alert">{{ error }} <button type="button" @click="sync()">重试</button></p>
    <p v-else-if="loaded" role="status">{{ enabled ? '已启用；关闭只停止后续自动安装，不卸载已有插件。' : '当前关闭。开启后可让猫猫在上述权限范围内自己创建工具。' }}</p>
  </section>
</template>

<style scoped>
.author-panel { display:flex; flex-direction:column; gap:12px; margin:16px 0; padding:20px; }
.author-heading { display:flex; align-items:flex-start; justify-content:space-between; flex-wrap:wrap; gap:16px; }
.author-heading>div { flex:1; min-width:min(250px,100%); }
h3 { margin:0 0 8px; font-size:18px; }p { margin:0; line-height:1.6; }
label { display:flex; align-items:center; gap:10px; min-height:44px; }
.scope,[role=status] { color:var(--md-on-surface-variant); }
.examples { display:flex; flex-wrap:wrap; gap:8px; }
[role=alert] { color:var(--md-error); }button { font:inherit; }
</style>
