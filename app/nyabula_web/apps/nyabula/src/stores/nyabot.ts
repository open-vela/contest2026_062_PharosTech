/* Core owns executions. Drafts and selection stay in memory. */
import { defineStore } from 'pinia';
import { computed, ref, watch } from 'vue';
import { useSessionStore } from './session';

export interface NyabotRun {
  id: string; requestId: string; conversationId: string; text: string; reply?: string;
  state: 'queued' | 'running' | 'waiting_approval' | 'cancelling' | 'succeeded' | 'failed' | 'cancelled' | 'unknown';
  createdAt: number; finishedAt?: number;
  caller?: string;
  steps?: { tool: string; topic: string; state: string; arguments?: Record<string, unknown>; error?: number; decision?: string; sideEffectApplied?: boolean; sideEffectUncertain?: boolean }[];
}
export interface NyabotStatus {
  ready: boolean; configured: boolean; streaming: boolean; lastError: number; activeRun: string;
}
export interface NyabotCapability { id: string; group: string; reason: string; compiled: boolean; enabled: boolean }
function identity(prefix: string): string {
  return prefix + Array.from(crypto.getRandomValues(new Uint8Array(16)), b => b.toString(16).padStart(2, '0')).join('');
}
function describeError(cause: unknown): string {
  const code = (cause as { code?: string })?.code ?? '';
  const messages: Record<string, string> = {
    ENOTCONFIGURED: '设备尚未配置可用模型。', EQUOTA: '设备会话空间不足，请删除不需要的会话后再试。',
    EPERM: '当前身份或连接不允许此操作。', EALREADY: '该请求所属会话已删除，不会重新执行。',
    ECONFLICT: '设备记录已更新，请刷新后重新确认。', ECANCELED: '操作已取消。',
  };
  return messages[code] ?? (cause instanceof Error ? cause.message : String(cause));
}
export const useNyabotStore = defineStore('nyabot', () => {
  const session = useSessionStore();
  const runs = ref<NyabotRun[]>([]);
  const revision = ref(0);
  const status = ref<NyabotStatus | null>(null);
  const capabilities = ref<NyabotCapability[]>([]);
  const draft = ref('');
  const drafts = new Map<string, string>();
  const conversation = ref('');
  const error = ref('');
  const loading = ref(false);
  const sending = ref(false);
  const uncertain = ref<{ requestId: string; conversationId: string; text: string } | null>(null);
  let epoch = 0;
  let users = 0;
  let timer: ReturnType<typeof setTimeout> | undefined;
  const active = computed(() => runs.value.filter(r => ['queued', 'running', 'waiting_approval', 'cancelling'].includes(r.state)));
  const decisions = ref(new Set<string>());
  const conversations = computed(() => {
    const items = new Map<string, { id: string; title: string }>();
    for (const run of runs.value) if (!items.has(run.conversationId)) items.set(run.conversationId, { id: run.conversationId, title: run.text });
    return [...items.values()].reverse();
  });
  const currentRuns = computed(() => runs.value.filter(r => r.conversationId === conversation.value));
  const canSend = computed(() => session.connected && session.isOwner && status.value?.configured &&
    status.value.ready && !status.value.activeRun && !sending.value && !uncertain.value && !!draft.value.trim());
  function newConversation(): void { conversation.value = identity('chat-'); draft.value = ''; }
  async function refresh(): Promise<void> {
    if (!session.connected || !session.isOwner || loading.value) return;
    const current = epoch;
    loading.value = true;
    try {
      const nextStatus = await session.request('agent.status', {});
      const response = await session.request('agent.runs.list', {});
      const available = capabilities.value.length ? null : await session.request('agent.capabilities', {});
      if (current !== epoch) return;
      status.value = nextStatus as unknown as NyabotStatus;
      if (available && Array.isArray(available.items)) capabilities.value = available.items as NyabotCapability[];
      runs.value = Array.isArray(response.runs) ? (response.runs as (NyabotRun & {state:string})[]).filter(r => String(r.state) !== 'deleted') : [];
      revision.value = Number(response.revision ?? 0);
      if (!conversation.value) conversation.value = conversations.value[0]?.id ?? identity('chat-');
      if (uncertain.value && runs.value.some(r => r.requestId === uncertain.value?.requestId)) {
        if (draft.value.trim() === uncertain.value.text) draft.value = '';
        uncertain.value = null;
      }
      error.value = '';
    } catch (cause) {
      if (current === epoch) error.value = describeError(cause);
    } finally { if (current === epoch) loading.value = false; }
  }
  function schedule(): void {
    clearTimeout(timer);
    if (users > 0) timer = setTimeout(async () => { await refresh(); schedule(); },
      active.value.length || !status.value?.ready ? 1000 : 5000);
  }
  function retain(): () => void {
    users++; void refresh().finally(schedule);
    return () => { users--; if (!users) clearTimeout(timer); };
  }
  async function submit(text = draft.value, retry = false): Promise<void> {
    if (!session.connected || !session.isOwner || sending.value || !text.trim()) return;
    if (!retry && (!status.value?.configured || !status.value.ready || status.value.activeRun || uncertain.value)) return;
    const request = retry ? uncertain.value : {
      requestId: identity('req-'), conversationId: conversation.value || identity('chat-'), text: text.trim(),
    };
    if (!request) return;
    const current = epoch;
    conversation.value = request.conversationId;
    sending.value = true;
    try {
      await session.request('agent.chat', request);
      if (current !== epoch) return;
      uncertain.value = null;
      if (draft.value.trim() === request.text) draft.value = '';
      await refresh();
    } catch (cause) {
      if (current !== epoch) return;
      const code = (cause as { code?: string })?.code;
      if (!code || ['EOFFLINE', 'ETIMEOUT', 'ETIMEDOUT', 'ECLOSED', 'ECONNRESET'].includes(code)) uncertain.value = request;
      error.value = describeError(cause);
    } finally { if (current === epoch) { sending.value = false; schedule(); } }
  }
  async function cancel(id: string): Promise<void> {
    const current = epoch;
    try { await session.request('agent.cancel', { id }); if (current === epoch) await refresh(); }
    catch (cause) { if (current === epoch) error.value = describeError(cause); }
  }
  async function decide(runId: string, step: number, decision: 'allow' | 'deny'): Promise<void> {
    const id = `${runId}:${step}`;
    if (decisions.value.has(id) || !session.connected || !session.isOwner) return;
    const current = epoch;
    decisions.value.add(id);
    try { await session.request('agent.approval.decide', { runId, step, decision }); if (current === epoch) await refresh(); }
    catch (cause) { if (current === epoch) error.value = describeError(cause); }
    finally { decisions.value.delete(id); }
  }
  /* Without an id the conversation on screen is the one deleted. */
  async function deleteConversation(id?: string): Promise<void> {
    const selected = id ?? conversation.value;
    if (!session.connected || !session.isOwner || !selected) return;
    const current = epoch;
    try {
      await session.request('agent.conversation.delete', { conversationId: selected, revision: revision.value });
      if (current !== epoch) return;
      drafts.delete(selected);
      if (conversation.value === selected) newConversation();
      drafts.delete(selected);
      await refresh();
    } catch (cause) { if (current === epoch) error.value = describeError(cause); }
  }
  watch(() => session.deviceKey, () => {
    epoch++; runs.value = []; revision.value = 0; status.value = null; capabilities.value = []; conversation.value = ''; draft.value = '';
    error.value = ''; uncertain.value = null; loading.value = false; sending.value = false; decisions.value.clear();
    drafts.clear();
  });
  watch(conversation, (next, previous) => {
    if (previous) drafts.set(previous, draft.value);
    draft.value = drafts.get(next) ?? '';
  }, { flush: 'sync' });
  watch(() => session.connected, connected => { if (connected && users) void refresh().finally(schedule); });
  return { runs, status, capabilities, draft, conversation, conversations, currentRuns, active,
    error, loading, sending, uncertain, canSend, decisions, refresh, retain, newConversation, submit, cancel, decide, deleteConversation };
});
