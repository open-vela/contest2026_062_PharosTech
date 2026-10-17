/* Device build only: who may use the page the device serves, and whether the
 * device is on a network yet. Owns the boot decision (pair token -> set/reset
 * the password, session token -> enter, otherwise log in) and the caches the
 * route guard reads, so no navigation waits on a round trip.
 * Never log a token or a password. */
import { defineStore } from 'pinia';
import { computed, ref } from 'vue';
import type { RouteLocationRaw } from 'vue-router';
import { SELF_KEY, useSessionStore } from './session';
import { isDeviceToken } from '../lib/deviceToken';
import { decideBoot, lockDeadline, parseAuthState, retryAfterOf, safeResumePath, type AccessPhase, type AuthState, type HelloOutcome } from '../lib/deviceAccess';
import { parseNetworkStatus, type NetworkStatus } from '../lib/wifi';

/** Lock length assumed when the device says "locked" without a duration. */
const LOCK_FALLBACK_MS = 30000;
/** A sys.auth.state this fresh is not fetched again (boot -> login page). */
const AUTH_STATE_FRESH_MS = 5000;

export function errorCode(e: unknown): string {
  const code = typeof e === 'object' && e !== null ? (e as { code?: unknown }).code : undefined;
  return typeof code === 'string' ? code : '';
}

function isAuthRejection(e: unknown): boolean {
  return ['EACCES', 'EPERM', 'EAUTH'].includes(errorCode(e));
}

export const useDeviceAccessStore = defineStore('deviceAccess', () => {
  const session = useSessionStore();

  const phase = ref<AccessPhase>('boot');
  const unreachable = ref(false);
  const setupMode = ref<'set' | 'reset'>('set');
  const authState = ref<AuthState | null>(null);
  /** Local deadline (epoch ms) until which sys.login is locked; 0 = open. */
  const lockUntil = ref(0);
  /** Cached network.status: true = sta_online, false = anything else, null = unknown. */
  const online = ref<boolean | null>(null);

  let authStateAt = 0;
  let pendingPath: string | null = null;
  let running: Promise<void> | null = null;

  const passwordSet = computed(() => session.passwordSet ?? authState.value?.passwordSet ?? null);

  /* ---------------- caches ---------------- */

  function noteNetwork(status: NetworkStatus | null): void {
    if (status) online.value = status.state === null ? null : status.state === 'sta_online';
  }

  async function refreshNetwork(): Promise<void> {
    try {
      noteNetwork(parseNetworkStatus(await session.request('network.status')));
    } catch {
      /* Keep the last known state; unknown never seals the app. */
    }
  }

  function noteAuthState(state: AuthState): void {
    authState.value = state;
    authStateAt = Date.now();
    lockUntil.value = lockDeadline(authStateAt, state.retryAfterMs || (state.locked ? LOCK_FALLBACK_MS : 0));
  }

  function noteLocked(errorData: unknown): void {
    lockUntil.value = lockDeadline(Date.now(), retryAfterOf(errorData) || LOCK_FALLBACK_MS);
  }

  /** Remember where the visitor was heading before the gate took over. */
  function rememberPath(fullPath: string): void {
    pendingPath = safeResumePath(fullPath) ?? pendingPath;
  }

  /** Where to go once access is granted (the guard still seals an offline device). */
  function nextLocation(): RouteLocationRaw {
    const path = pendingPath;
    pendingPath = null;
    return path ?? { name: 'home', params: { key: SELF_KEY } };
  }

  /* ---------------- boot ---------------- */

  async function runBoot(): Promise<void> {
    phase.value = 'boot';
    unreachable.value = false;
    session.disconnect();
    try {
      // Two rounds at most: a refused pair token falls back to a stored session.
      for (let round = 0; round < 2; round++) {
        noteAuthState(parseAuthState(await session.preAuth(SELF_KEY, 'sys.auth.state')));
        const input = { hasPairToken: session.hasPairToken, hasSessionToken: session.hasToken(SELF_KEY), passwordSet: authState.value?.passwordSet === true };
        let step = decideBoot({ ...input, helloResult: 'none' });
        if (step.action === 'hello') {
          let outcome: HelloOutcome;
          try {
            const hello = await session.authenticate(step.credential);
            if (typeof hello.passwordSet === 'boolean') input.passwordSet = hello.passwordSet;
            // "Pair with a code" is not a way in here: same as a refused credential.
            outcome = hello.pairingRequired ? 'rejected' : 'ok';
          } catch (e) {
            outcome = isAuthRejection(e) ? 'rejected' : 'failed';
          }
          step = decideBoot({ ...input, helloResult: outcome });
        }
        if (step.action === 'restart') {
          session.dropPairToken();
          session.disconnect();
          continue;
        }
        if (step.action === 'setup') {
          setupMode.value = step.mode;
          await refreshNetwork();
          phase.value = 'setup';
        } else if (step.action === 'enter') {
          await refreshNetwork();
          phase.value = 'ready';
        } else if (step.action === 'login') {
          if (step.forget === 'pair') session.dropPairToken();
          if (step.forget === 'session') session.forgetSessionToken(SELF_KEY);
          // The login page opens a socket when it needs one.
          session.disconnect();
          phase.value = 'login';
        } else {
          break;
        }
        return;
      }
    } catch {
      /* sys.auth.state did not get through: reported as unreachable below. */
    }
    session.disconnect();
    unreachable.value = true;
  }

  function boot(): Promise<void> {
    running ??= runBoot().finally(() => {
      running = null;
    });
    return running;
  }

  /** A fresh pair token arrived (QR link opened in a live tab): decide again. */
  function restart(): void {
    phase.value = 'boot';
    unreachable.value = false;
  }

  /* ---------------- login ---------------- */

  /** Login page: make sure the lock state is known, without holding a socket. */
  async function ensureAuthState(force = false): Promise<void> {
    if (!force && authState.value && Date.now() - authStateAt < AUTH_STATE_FRESH_MS) return;
    try {
      noteAuthState(parseAuthState(await session.preAuth(SELF_KEY, 'sys.auth.state')));
    } finally {
      session.releasePreAuth();
    }
  }

  /** sys.login -> store the session token -> hello. Rejects with the device
   *  error (EAUTH / ELOCKED / ENOPASSWORD) or a link error. */
  async function login(password: string): Promise<void> {
    try {
      const res = await session.preAuth(SELF_KEY, 'sys.login', { password });
      if (!isDeviceToken(res.token) || !session.storeSessionToken(SELF_KEY, res.token)) {
        throw Object.assign(new Error('设备返回的登录结果无效'), { code: 'EPROTO' });
      }
      const hello = await session.authenticate('session');
      if (hello.pairingRequired) throw Object.assign(new Error('设备没有接受这次登录，请再试一次'), { code: 'EPROTO' });
    } catch (e) {
      const code = errorCode(e);
      if (code === 'ELOCKED') noteLocked((e as { data?: unknown }).data);
      else if (code === 'EAUTH') {
        // The socket survives a wrong password: learn whether it is locked now.
        try {
          noteAuthState(parseAuthState(await session.preAuth(SELF_KEY, 'sys.auth.state')));
        } catch {
          /* best effort */
        }
      } else if (code === 'ENOPASSWORD') noteAuthState({ passwordSet: false, locked: false, retryAfterMs: 0 });
      // Nothing worth keeping: the next attempt opens its own socket.
      session.disconnect();
      throw e;
    }
    await refreshNetwork();
    phase.value = 'ready';
  }

  /* ---------------- password ---------------- */

  /** sys.password.set on the authenticated socket. `current` is required by
   *  the device when the socket said hello with a session token. */
  async function setPassword(password: string, current?: string): Promise<void> {
    const res = await session.request('sys.password.set', current === undefined ? { password } : { password, current });
    const key = session.deviceKey ?? SELF_KEY;
    if (!isDeviceToken(res.token) || !session.storeSessionToken(key, res.token)) {
      throw Object.assign(new Error('设备返回的结果无效'), { code: 'EPROTO' });
    }
    // The stored session replaces the pair token from here on.
    session.dropPairToken();
    session.markPasswordSet();
    if (authState.value) authState.value = { ...authState.value, passwordSet: true };
  }

  /** Setup page done (password set, or "enter without changing"). */
  function finishSetup(): void {
    if (phase.value === 'setup') phase.value = 'ready';
  }

  /** The device refused the credential of a live session (another client
   *  changed the password, the device was reset). Returns the gate to show. */
  function handleAuthLost(): 'connect' | 'login' | null {
    if (phase.value === 'boot' || phase.value === 'login') return null;
    const kind = session.credential;
    if (kind === 'pair') session.dropPairToken();
    // Another tab may already have stored the new session token: try that first.
    const remains = kind === 'session' ? session.forgetSessionToken(SELF_KEY, true) : session.hasToken(SELF_KEY);
    session.disconnect();
    unreachable.value = false;
    phase.value = remains ? 'boot' : 'login';
    return remains ? 'connect' : 'login';
  }

  /** Authenticated phase but nothing left to say hello with (site data
   *  cleared under a live tab): the only way forward is the login page. */
  function requireLogin(): boolean {
    if (phase.value !== 'ready' || session.hasPairToken || session.hasToken(SELF_KEY)) return false;
    session.disconnect();
    phase.value = 'login';
    return true;
  }

  return {
    phase, unreachable, setupMode, authState, lockUntil, online, passwordSet,
    noteNetwork, refreshNetwork, rememberPath, nextLocation,
    boot, restart, ensureAuthState, login, setPassword, finishSetup, handleAuthLost, requireLogin,
  };
});
