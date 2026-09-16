import { computed, onMounted, onUnmounted } from 'vue';
import { useProductStore } from '../stores/product';

export interface Alarm {
  id: string; time: string; label: string; repeat: string[]; enabled: boolean;
  utc_offset_minutes: number; next_at: number; status: string;
}

export function useAlarms() {
  const product = useProductStore();
  const alarms = computed(() => (product.records.alarm?.items ?? []) as unknown as Alarm[]);
  const sorted = computed(() => [...alarms.value].sort((a, b) => a.time.localeCompare(b.time)));
  const nextAlarm = computed(() => alarms.value.filter(a => a.next_at > 0)
    .sort((a, b) => a.next_at - b.next_at)[0] ?? null);
  let polling: ReturnType<typeof setInterval> | undefined;
  onMounted(() => {
    void product.refresh('alarm');
    polling = setInterval(() => void product.refresh('alarm', true), 2000);
  });
  onUnmounted(() => clearInterval(polling));
  return { product, alarms, sorted, nextAlarm };
}
