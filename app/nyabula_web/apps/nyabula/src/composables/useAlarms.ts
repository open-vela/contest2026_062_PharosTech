import { computed, watch } from 'vue';
import { useProductStore, type ProductRecord } from '../stores/product';
import { useSessionStore } from '../stores/session';
import { useProductRecords } from './useProductRecords';
import { useEyeScene } from './useEyeScene';
import { alarmScene } from './eyeScenePayload';

export interface Alarm extends ProductRecord {
  id: string; time: string; label: string; repeat: string[]; enabled: boolean;
  utc_offset_minutes: number; next_at: number; status: string;
}

/** Alarms are Core records; ringing is a durable status there
 *  (scheduled | ringing | snoozed | dismissed | missed). */
export function useAlarms() {
  const product = useProductStore();
  const session = useSessionStore();
  const core = useProductRecords<Alarm>('alarm', 2000);
  const alarms = computed(() => core.items.value);
  const sorted = computed(() => [...alarms.value].sort((a, b) => a.time.localeCompare(b.time)));
  const nextAlarm = computed(() => alarms.value.filter(a => a.next_at > 0)
    .sort((a, b) => a.next_at - b.next_at)[0] ?? null);
  const ringing = computed(() => alarms.value.filter(a => a.status === 'ringing'));

  /* Eye link: the ringing alarm if any, otherwise the next one. */
  const scene = useEyeScene('alarm', () => {
    const a = ringing.value[0] ?? nextAlarm.value;
    return a ? alarmScene({ time: a.time, label: a.label, ringing: a.status === 'ringing' }) : null;
  }, { hideWhenEmpty: true });

  /* A ring puts the alarm on the eyes by itself; when it ends, only a scene
   * this watcher put there is taken down again. */
  let autoShown = false;
  watch(() => ringing.value[0]?.id, (id, old) => {
    if (id && id !== old && session.canControl) {
      if (!scene.held.value) autoShown = true;
      void scene.show();
    } else if (!id && old && autoShown) {
      autoShown = false;
      if (scene.held.value) void scene.hide();
    }
  }, { immediate: true });

  const dismiss = (a: Alarm): Promise<boolean> => core.mutate('dismiss', { id: a.id });
  const snooze = (a: Alarm): Promise<boolean> => core.mutate('snooze', { id: a.id });
  return { product, core, alarms, sorted, nextAlarm, ringing, scene, dismiss, snooze };
}
