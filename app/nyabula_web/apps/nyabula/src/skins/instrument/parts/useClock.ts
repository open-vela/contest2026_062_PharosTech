/* Shared wall clock for instrument readouts (one interval for all users). */
import { onBeforeUnmount, onMounted, ref } from 'vue';

const now = ref(new Date());
let users = 0;
let timer: number | null = null;

export function useClock() {
  onMounted(() => {
    if (users++ === 0) timer = window.setInterval(() => (now.value = new Date()), 1000);
  });
  onBeforeUnmount(() => {
    if (--users === 0 && timer !== null) {
      window.clearInterval(timer);
      timer = null;
    }
  });
  const pad = (n: number) => String(n).padStart(2, '0');
  const hhmm = () => `${pad(now.value.getHours())}:${pad(now.value.getMinutes())}`;
  const colonOn = () => now.value.getSeconds() % 2 === 0;
  return { now, hhmm, colonOn };
}
