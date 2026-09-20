/* Shared view adapter; no local execution state or fabricated token stream. */
import { computed, onBeforeUnmount } from 'vue';
import { storeToRefs } from 'pinia';
import { useRoute } from 'vue-router';
import { useSessionStore } from '../../stores/session';
import { useNyabotStore } from '../../stores/nyabot';

export interface ChatMessage {
  id: string; role: 'user' | 'agent'; text: string; pending: boolean;
  unsupported?: boolean; error?: string; at: number;
}
export const RUN_LABELS: Record<string, string> = {
  queued: '已排队', running: '执行中', cancelling: '取消中，等待执行确认', succeeded: '已完成',
  waiting_approval: '等待主人批准操作',
  failed: '执行失败', cancelled: '已取消，已发生的操作未回滚', unknown: '结果未知，请核对后再决定是否重试',
};
export const QUICK_PROMPTS = ['你好，openvela', '现在几点了？', '讲个笑话'];

export function useAgentPage() {
  const session = useSessionStore();
  const bot = useNyabotStore();
  const { draft, sending, canSend } = storeToRefs(bot);
  const stop = bot.retain();
  onBeforeUnmount(stop);
  const route = useRoute();
  if (typeof route.query.q === 'string' && !draft.value) {
    bot.newConversation();
    draft.value = route.query.q;
  }
  const messages = computed<ChatMessage[]>(() => bot.currentRuns.flatMap(run => [
    { id: run.id + '-user', role: 'user', text: run.text, pending: false, at: run.createdAt },
    { id: run.id, role: 'agent', text: ['cancelled', 'unknown'].includes(run.state)
        ? RUN_LABELS[run.state] : run.reply || RUN_LABELS[run.state] || run.state,
      pending: ['queued', 'running', 'cancelling'].includes(run.state),
      error: run.state === 'failed' ? run.reply || '执行失败' : undefined, at: run.finishedAt ?? run.createdAt },
  ]));
  return { session, bot, messages, draft, sending, canSend,
    unsupported: computed(() => false), send: bot.submit, clear: bot.newConversation, quickPrompts: QUICK_PROMPTS };
}
