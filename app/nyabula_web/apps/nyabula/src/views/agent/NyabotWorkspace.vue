<script setup lang="ts">
import { computed } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { MdButton, UiIcon, useDialogStore } from '@nyabula/ui';
import ChatThread from './ChatThread.vue';
import ChatComposer from './ChatComposer.vue';
import ModelConfigPanel from './ModelConfigPanel.vue';
import ApprovalList from './ApprovalList.vue';
import SkillsPanel from './SkillsPanel.vue';
import McpPanel from './McpPanel.vue';
import AutomationPanel from './AutomationPanel.vue';
import ChannelsPanel from './ChannelsPanel.vue';
import ToolsPanel from './ToolsPanel.vue';
import NodePanel from './NodePanel.vue';
import { RUN_LABELS, useAgentPage } from './agent.logic';
import { CAPABILITY_LABELS, CAPABILITY_REASONS } from './capabilities';

const page = useAgentPage();
const bot = page.bot;
const route = useRoute();
const router = useRouter();
const dialog = useDialogStore();
async function deleteConversation(): Promise<void> {
  if (await dialog.confirm('删除当前会话的文本和执行细节，保留请求标识以防重复执行。关联的记忆、待办和计时器不会删除。',
    { title: '删除设备端会话', danger: true, confirmText: '删除会话' })) await bot.deleteConversation();
}
const sections = [
  { id: 'chat', label: '聊天' }, { id: 'tasks', label: '任务' }, { id: 'memory', label: '记忆' },
  { id: 'capabilities', label: '能力与连接' }, { id: 'config', label: '配置' },
  { id: 'skills', label: 'Skills' },
  { id: 'automation', label: '自动化' },
  { id: 'mcp', label: 'MCP' },
  { id: 'channels', label: '消息渠道' },
  { id: 'tools', label: '工具' },
  { id: 'node', label: 'OpenClaw' },
];
const section = computed(() => sections.some(s => s.id === route.query.tab) ? String(route.query.tab) : 'chat');
function select(id: string): void { void router.replace({ query: { ...route.query, tab: id } }); }
const stateLabel = computed(() => !page.session.connected ? '设备未连接' : !page.session.isOwner ? '需要主人权限' :
  !bot.status ? '正在读取 Nyabot 状态' : !bot.status.ready ? '正在启动' :
  !bot.status.configured ? '还没有可用模型' : '可对话');
</script>

<template>
  <div class="nyabot-workspace">
    <nav class="section-nav" aria-label="Nyabot 工作区">
      <button v-for="item in sections" :key="item.id" :aria-current="section === item.id ? 'page' : undefined"
        :class="{ selected: section === item.id }" @click="select(item.id)">{{ item.label }}</button>
    </nav>
    <p v-if="bot.error" class="error" role="alert">{{ bot.error }} <button @click="bot.refresh()">重新读取</button></p>
    <div v-if="section === 'chat'" class="chat-layout">
      <aside class="conversations" aria-label="会话列表">
        <MdButton variant="tonal" @click="bot.newConversation()"><UiIcon name="add" :size="18" />新建会话</MdButton>
        <button v-for="item in bot.conversations" :key="item.id" class="conversation" :class="{ selected: bot.conversation === item.id }"
          @click="bot.conversation = item.id">{{ item.title }}</button>
        <p v-if="!bot.conversations.length" class="muted">尚无设备端会话</p>
      </aside>
      <section class="chat-content" :class="{ 'awaiting-approval': bot.currentRuns.some(r => r.steps?.some(s => s.state === 'pending')) }" aria-label="当前会话">
        <div class="chat-state"><span>{{ stateLabel }}</span><MdButton v-if="!bot.status?.configured" variant="text" @click="select('config')">模型配置</MdButton>
          <MdButton v-else-if="bot.currentRuns.length" variant="text" :disabled="!page.session.connected || bot.active.some(r => r.conversationId === bot.conversation)" @click="deleteConversation">删除会话</MdButton>
        </div>
        <ChatThread :messages="page.messages.value" :connected="page.session.connected" class="chat-messages" />
        <ApprovalList :runs="bot.currentRuns" />
        <div v-if="bot.uncertain" class="notice" role="status">
          发送结果尚未确认。重连后会核对原请求，不会自动重复执行。
          <MdButton variant="text" @click="bot.refresh()">核对结果</MdButton>
          <MdButton variant="text" :disabled="!page.session.connected || bot.sending" @click="bot.submit(bot.uncertain.text, true)">重试原请求</MdButton>
        </div>
        <div v-for="run in bot.currentRuns.filter(r => ['queued', 'running', 'waiting_approval', 'cancelling'].includes(r.state))" :key="run.id" class="execution">
          <span>{{ RUN_LABELS[run.state] }}</span>
          <MdButton variant="text" :disabled="run.state === 'cancelling' || !page.session.connected" @click="bot.cancel(run.id)">取消任务</MdButton>
        </div>
        <ChatComposer :page="page" />
        <p class="muted composer-hint">{{ bot.status?.streaming ? '按设备实际输出更新' : '等待设备完成后显示完整回复' }} · 你好，openvela</p>
      </section>
    </div>
    <section v-else-if="section === 'tasks'" class="panel">
      <h2>执行任务</h2><p class="muted">这里是 Nyabot 的真实运行记录。人工待办与自动化规则不是执行进度。</p>
      <MdButton variant="tonal" @click="select('automation')">管理自动化</MdButton>
      <ApprovalList :runs="bot.runs" />
      <p v-if="!bot.runs.length">暂无执行记录</p>
      <article v-for="run in [...bot.runs].reverse()" :key="run.id" class="run">
        <div><strong>{{ run.text }}</strong><p>{{ RUN_LABELS[run.state] }}</p><code>{{ run.id }}</code>
          <details v-if="run.steps?.length"><summary>执行步骤 · {{ run.steps.length }}</summary>
            <ol><li v-for="(step, index) in run.steps" :key="index">
              {{ step.topic }} · {{ step.state }}
              <strong v-if="step.sideEffectUncertain"> · 远端副作用不确定，请核对，勿重试</strong>
              <strong v-else-if="step.sideEffectApplied"> · {{ step.topic === 'agent.mcp.out.call' ? '远端已返回成功' : '已修改设备记录' }}</strong>
              <span v-if="step.error"> · 错误 {{ step.error }}</span>
            </li></ol>
          </details>
        </div>
        <MdButton variant="text" @click="bot.conversation = run.conversationId; select('chat')">打开会话</MdButton>
        <MdButton v-if="['queued', 'running', 'waiting_approval'].includes(run.state)" variant="text" :disabled="!page.session.connected" @click="bot.cancel(run.id)">取消任务</MdButton>
      </article>
      <RouterLink :to="{ name: 'service', params: { key: route.params.key, type: 'task' } }">查看人工待办</RouterLink>
    </section>
    <section v-else-if="section === 'memory'" class="panel">
      <h2>记忆中心</h2><p>事实与偏好使用设备上已有的记忆记录，不在聊天页面保存第二份。</p>
      <RouterLink :to="{ name: 'service', params: { key: route.params.key, type: 'memory' } }">打开设备记忆</RouterLink>
    </section>
    <section v-else-if="section === 'capabilities'" class="panel">
      <h2>能力与连接</h2><p>模型、Skills、工具、MCP、节点和消息渠道分别配置。消息渠道需手动启用，未接入的能力会注明原因。</p>
      <div class="capabilities">
        <article v-for="capability in bot.capabilities" :key="capability.id" class="capability">
          <strong>{{ CAPABILITY_LABELS[capability.id] ?? capability.id }}</strong>
          <span>{{ capability.enabled ? '已接入' : capability.compiled ? '已编译 · 未启用' : '当前不可用' }}</span>
          <p class="muted">{{ CAPABILITY_REASONS[capability.reason] ?? capability.reason }}</p>
          <MdButton v-if="capability.id === 'skills' && capability.enabled" variant="text" @click="select('skills')">管理 Skills</MdButton>
          <MdButton v-if="capability.id === 'mqtt' && capability.enabled" variant="text" @click="select('channels')">管理 MQTT</MdButton>
          <MdButton v-if="capability.id === 'mcp-client' && capability.enabled" variant="text" @click="select('mcp')">管理 MCP 外连</MdButton>
          <MdButton v-if="capability.id === 'mcp-server' && capability.enabled" variant="text" @click="select('mcp')">管理 MCP 来访</MdButton>
        </article>
      </div>
      <p v-if="!bot.capabilities.length" class="muted">尚未取得设备能力清单。</p>
      <RouterLink :to="{ name: 'plugins', params: { key: route.params.key } }">管理 Core 扩展</RouterLink>
    </section>
    <SkillsPanel v-else-if="section === 'skills'" />
    <ChannelsPanel v-else-if="section === 'channels'" />
    <ToolsPanel v-else-if="section === 'tools'" />
    <NodePanel v-else-if="section === 'node'" />
    <McpPanel v-else-if="section === 'mcp'" />
    <AutomationPanel v-else-if="section === 'automation'" @open="id => { bot.conversation = id; select('chat'); }" />
    <ModelConfigPanel v-else class="panel" />
  </div>
</template>

<style scoped>
.nyabot-workspace { display: flex; flex-direction: column; min-height: 100%; padding: 16px 24px; box-sizing: border-box; gap: 16px; }
.section-nav { display: flex; gap: 6px; overflow-x: auto; border-bottom: 1px solid var(--md-outline-variant); flex: none; }
.section-nav button, .conversation { border: 0; background: transparent; color: var(--md-on-surface-variant); font: 500 15px var(--font-body); cursor: pointer; min-height: 44px; padding: 8px 14px; white-space: nowrap; }
.section-nav .selected { color: var(--md-primary); border-bottom: 2px solid var(--md-primary); }
.chat-layout { display: grid; grid-template-columns: 220px minmax(0, 1fr); gap: 24px; flex: 1; min-height: 560px; }
.conversations { display: flex; flex-direction: column; gap: 8px; min-width: 0; border-right: 1px solid var(--md-outline-variant); padding-right: 16px; }
.conversation { text-align: left; overflow: hidden; text-overflow: ellipsis; border-radius: 12px; }
.conversation.selected { background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.chat-content { display: flex; flex-direction: column; gap: 12px; min-width: 0; max-width: 960px; }
.chat-state, .execution { display: flex; align-items: center; justify-content: space-between; gap: 12px; min-height: 44px; }
.chat-messages { flex: 1; min-height: 300px; max-height: 60dvh; }
.composer-hint { font-size: 12px; margin: 0; }
.panel { max-width: 960px; width: 100%; margin: 0 auto; line-height: 1.6; }
.panel h2 { font: 600 22px var(--font-title); }
.run { display: flex; flex-wrap: wrap; gap: 12px; padding: 16px 0; border-bottom: 1px solid var(--md-outline-variant); }
.run > div { flex: 1; min-width: 180px; overflow-wrap: anywhere; }
.run p { margin: 6px 0; }
.notice { padding: 12px; background: var(--md-surface-container); border-radius: 12px; }
.capabilities { display: grid; grid-template-columns: repeat(auto-fit, minmax(230px, 1fr)); gap: 20px; margin: 20px 0; }
.capability { display: flex; flex-direction: column; gap: 5px; border-bottom: 1px solid var(--md-outline-variant); padding-bottom: 12px; }
.capability span { font-size: 13px; }
.capability p { margin: 0; }
.error { color: var(--md-error); overflow-wrap: anywhere; }
@media (max-width: 700px) {
  .nyabot-workspace { padding: 8px 12px 16px; }
  .chat-layout { display: flex; flex-direction: column; gap: 12px; min-height: 520px; }
  .conversations { flex-direction: row; overflow-x: auto; border-right: 0; padding: 0; }
  .conversations > * { flex: none; max-width: 180px; }
  .chat-content { flex: 1; }
  .awaiting-approval .chat-messages { flex: none; min-height: 100px; max-height: 180px; }
  .section-nav button { padding: 8px 10px; }
}
</style>
