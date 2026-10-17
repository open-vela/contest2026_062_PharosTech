import { computed, ref, watch } from 'vue';
import { defineStore } from 'pinia';
import { useSessionStore } from './session';

export interface BriefingItem { id:string; title:string; text:string; source:string }
export interface BriefingState { revision:number; generated_at?:number; items:BriefingItem[]; playing:boolean; index:number; next_at:number;
  schedule_enabled:boolean; morning_minute:number; evening_minute:number; utc_offset_minutes:number }

export const useBriefingStore = defineStore('briefing', () => {
  const session = useSessionStore();
  const state = ref<BriefingState | null>(null);
  const error = ref('');
  const busy = ref(false);
  const available = computed(() => session.connected && session.client?.capabilities.includes('core.briefing-v1') === true);
  let generation = 0;
  let pending: Promise<void> | null = null;
  watch(() => [session.client, session.state], () => { generation++; state.value = null; error.value = ''; pending = null; });
  function parse(value: Record<string, unknown>): BriefingState {
    if (!Number.isSafeInteger(value.revision) || !Array.isArray(value.items) ||
        !value.items.every(item => item && typeof item === 'object' && typeof item.id === 'string' &&
          typeof item.title === 'string' && typeof item.text === 'string' && typeof item.source === 'string'))
      throw new Error('设备返回的简报格式无效');
    return value as unknown as BriefingState;
  }
  function refresh(): Promise<void> {
    if (!available.value) return Promise.resolve();
    if (pending) return pending;
    const current = generation;
    pending = session.request('briefing.get').then(value => {
      if (current === generation) { state.value = parse(value); error.value = ''; }
    }).catch(e => { if (current === generation) error.value = e instanceof Error ? e.message : String(e); })
      .finally(() => { if (current === generation) pending = null; });
    return pending;
  }
  async function act(operation: 'generate'|'configure'|'start'|'stop'|'next', data:Record<string,unknown> = {}): Promise<boolean> {
    if (!available.value || busy.value) return false;
    const current = generation;
    busy.value = true;
    try {
      if (!state.value) await refresh();
      if (!state.value || current !== generation) return false;
      state.value = parse(await session.request(`briefing.${operation}`, { ...data, revision:state.value.revision }));
      error.value = '';
      return true;
    } catch (e) { if (current === generation) error.value = e instanceof Error ? e.message : String(e); return false; }
    finally { if (current === generation) busy.value = false; }
  }
  return { state, error, busy, available, refresh, act };
});
