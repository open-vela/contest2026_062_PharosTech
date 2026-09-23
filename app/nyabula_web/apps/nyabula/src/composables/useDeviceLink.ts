/* Opens the session for a device key outside the /d/:key tree (boot view,
 * provisioning page) and reduces the first connection attempt to one gate. */
import { onBeforeUnmount, ref, watch } from 'vue';
import { useSessionStore } from '../stores/session';

/** connecting -> ready | pairing (no usable token) | auth (token rejected) | unreachable */
export type LinkGate = 'connecting' | 'ready' | 'pairing' | 'auth' | 'unreachable';

const CONNECT_TIMEOUT_MS = 8000;

export function useDeviceLink(key: string) {
  const session = useSessionStore();
  const gate = ref<LinkGate>('connecting');
  let timer: number | undefined;

  function evaluate(): void {
    if (session.deviceKey !== key) return;
    if (session.state === 'connected') gate.value = 'ready';
    else if (session.state === 'pairing-required') gate.value = 'pairing';
    else if (session.authRequired) gate.value = 'auth';
    // Later drops keep the gate: pages handle a lost link themselves.
  }

  function start(): void {
    gate.value = 'connecting';
    window.clearTimeout(timer);
    timer = window.setTimeout(() => {
      if (gate.value === 'connecting') gate.value = 'unreachable';
    }, CONNECT_TIMEOUT_MS);
    if (!session.connect(key)) gate.value = 'unreachable';
    else evaluate(); // connect() is a no-op when this key is already live
  }

  const stop = watch(() => [session.deviceKey, session.state, session.authRequired], evaluate);

  start();
  onBeforeUnmount(() => {
    stop();
    window.clearTimeout(timer);
  });

  return { session, gate, retry: start };
}
