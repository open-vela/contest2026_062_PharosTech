/* Device build only, mounted once by App.vue: reacts to what the route guard
 * cannot see because no navigation happens.
 *  - the device refuses the credential of a live session -> /login (or the
 *    boot view when another tab already stored a newer session token);
 *  - a socket opens with no credential left to say hello with -> /login;
 *  - the link (re)connects -> refresh the cached network state the guard reads. */
import { watch } from 'vue';
import { useRouter } from 'vue-router';
import { useSessionStore } from '../stores/session';
import { useDeviceAccessStore } from '../stores/deviceAccess';

export function useDeviceAccessGuard(): void {
  const router = useRouter();
  const session = useSessionStore();
  const access = useDeviceAccessStore();

  function authLost(): void {
    const gate = access.handleAuthLost();
    if (!gate) return;
    // Come back to the same page once access is granted again.
    access.rememberPath(router.currentRoute.value.fullPath);
    void router.replace({ name: gate });
  }

  watch(
    () => session.authRequired,
    (lost) => {
      if (lost) authLost();
    },
  );

  /* A socket that (re)opens with no credential at hand would wait for a hello
   * nobody sends and then sit there as "disconnected": go to /login instead. */
  watch(
    () => session.state,
    (state) => {
      // The pairing-code overlay is a hosted-build flow; here it means "not let in".
      if (state === 'pairing-required') return authLost();
      if (state !== 'authenticating' && state !== 'closed') return;
      if (!access.requireLogin()) return;
      access.rememberPath(router.currentRoute.value.fullPath);
      void router.replace({ name: 'login' });
    },
  );

  watch(
    () => session.connected,
    (connected) => {
      if (connected && access.phase === 'ready') void access.refreshNetwork();
    },
  );
}
