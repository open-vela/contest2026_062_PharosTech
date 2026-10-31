import { computed, ref, watch } from 'vue';
import { defineStore } from 'pinia';
import { useSessionStore } from './session';

export interface CompanionState {
  revision:number; enabled:boolean; mode:'quiet'|'interactive'|'story'; quiet_start:number; quiet_end:number;
  utc_offset_minutes:number; minimum_interval_minutes:number; daily_limit:number; daily_count:number;
  last_at:number; next_at:number; last_error:number; pending_run:string; quiet_now:boolean;
}

export const useCompanionStore = defineStore('companion', () => {
  const session = useSessionStore();
  const state = ref<CompanionState | null>(null);
  const error = ref('');
  const busy = ref(false);
  const available = computed(() => session.connected && session.client?.capabilities.includes('core.companion-v1') === true);
  let generation = 0;
  watch(() => [session.client, session.state], () => { generation++; state.value = null; error.value = ''; });
  async function refresh(): Promise<void> {
    if (!available.value) return;
    const current = generation;
    try { const value = await session.request('companion.get'); if (current === generation) state.value = value as unknown as CompanionState; }
    catch (e) { if (current === generation) error.value = e instanceof Error ? e.message : String(e); }
  }
  async function act(operation:'configure'|'run', data:Record<string,unknown>): Promise<boolean> {
    if (!available.value || busy.value) return false;
    const current = generation; busy.value = true;
    try {
      if (!state.value) await refresh();
      if (!state.value || current !== generation) return false;
      state.value = await session.request(`companion.${operation}`, { ...data, revision:state.value.revision }) as unknown as CompanionState;
      error.value = ''; return true;
    } catch (e) { if (current === generation) error.value = e instanceof Error ? e.message : String(e); return false; }
    finally { if (current === generation) busy.value = false; }
  }
  return { state, error, busy, available, refresh, act };
});
