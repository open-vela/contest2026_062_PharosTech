<script setup lang="ts">
import { computed } from 'vue';
import { MdButton } from '@nyabula/ui';
import { useNyabotStore, type NyabotRun } from '../../stores/nyabot';
import { useSessionStore } from '../../stores/session';

const props = defineProps<{ runs: NyabotRun[] }>();
const bot = useNyabotStore();
const session = useSessionStore();
const pending = computed(() => props.runs.flatMap(run => (run.steps ?? []).map((step, index) => ({ run, step, index }))
  .filter(item => item.step.state === 'pending')));
const labels: Record<string, string> = { 'memory.create': '记住一条信息', 'task.create': '创建待办',
  'calendar.create': '创建日历事项', 'timer.create': '创建计时任务', 'agent.mcp.out.call': '调用外部 MCP 服务' };
</script>

<template>
  <section v-for="item in pending" :key="`${item.run.id}:${item.index}`" class="approval" aria-label="待批准操作">
    <h3>{{ labels[item.step.topic] ?? item.step.topic }}</h3>
    <p v-if="item.step.topic === 'agent.mcp.out.call'">Nyabot 提议 · 外部服务 {{ item.step.arguments?.server }} · {{ item.step.arguments?.kind }} / {{ item.step.arguments?.name }}</p>
    <p v-else>Nyabot 提议 · 执行目标：当前设备 Core</p>
    <p class="muted">发起标识 {{ item.run.caller }} · {{ item.run.id }}</p>
    <pre>{{ JSON.stringify(item.step.arguments, null, 2) }}</pre>
    <p>仅授权上述参数的一次操作；拒绝不会修改记录。模型仍可能收到操作结果。</p>
    <p v-if="item.step.topic === 'agent.mcp.out.call'">参数会发送到此外部服务，可能产生远端副作用。批准后仍会核对目录版本与授权；派发后的请求无法靠撤销授权收回。</p>
    <div class="actions">
      <MdButton :disabled="!session.connected || !session.isOwner || bot.decisions.has(`${item.run.id}:${item.index}`)"
        @click="bot.decide(item.run.id, item.index, 'allow')">允许一次</MdButton>
      <MdButton variant="outlined" :disabled="!session.connected || !session.isOwner || bot.decisions.has(`${item.run.id}:${item.index}`)"
        @click="bot.decide(item.run.id, item.index, 'deny')">拒绝</MdButton>
    </div>
  </section>
</template>

<style scoped>
.approval { border: 1px solid var(--md-outline); border-radius: 16px; padding: 16px; margin: 8px 0; background: var(--md-surface-container); }
h3 { margin: 0; font: 600 18px var(--font-title); }
p { font-size: 14px; overflow-wrap: anywhere; }
pre { white-space: pre-wrap; overflow-wrap: anywhere; font-size: 13px; max-height: 240px; overflow: auto; }
.actions { display: flex; flex-wrap: wrap; gap: 10px; }
</style>
