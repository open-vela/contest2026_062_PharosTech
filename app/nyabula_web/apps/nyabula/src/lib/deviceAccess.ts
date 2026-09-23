/* Device-build access control (pure, no framework imports so it is
 * unit-testable): the boot decision, the route-guard decision, password
 * validation and the login lock countdown. Never log a token or a password. */

/* ---------------- boot decision ---------------- */

/** none = hello not attempted yet; rejected = EACCES & co; failed = link trouble. */
export type HelloOutcome = 'none' | 'ok' | 'rejected' | 'failed';

export interface BootInput {
  /** The tab holds a pair token taken from the QR-code URL (memory only). */
  hasPairToken: boolean;
  /** A session token is stored for this origin. */
  hasSessionToken: boolean;
  /** `passwordSet` from sys.auth.state / sys.hello. */
  passwordSet: boolean;
  helloResult: HelloOutcome;
}

export type BootStep =
  /** Say hello with the named credential, then decide again with its outcome. */
  | { action: 'hello'; credential: 'pair' | 'session' }
  /** Pair token accepted: set (no password yet) or reset the password. */
  | { action: 'setup'; mode: 'set' | 'reset' }
  /** Session token accepted. */
  | { action: 'enter' }
  /** Ask for the password; `forget` names the credential that was refused. */
  | { action: 'login'; forget: 'pair' | 'session' | null }
  /** The pair token was refused but a stored session is worth a try. */
  | { action: 'restart'; forget: 'pair' }
  | { action: 'unreachable' };

/** A pair token always wins: scanning the code is what allows a (re)set. */
export function decideBoot(input: BootInput): BootStep {
  const credential = input.hasPairToken ? 'pair' : input.hasSessionToken ? 'session' : null;
  if (!credential) return { action: 'login', forget: null };
  switch (input.helloResult) {
    case 'none':
      return { action: 'hello', credential };
    case 'failed':
      return { action: 'unreachable' };
    case 'ok':
      return credential === 'pair' ? { action: 'setup', mode: input.passwordSet ? 'reset' : 'set' } : { action: 'enter' };
    case 'rejected':
      if (credential === 'session') return { action: 'login', forget: 'session' };
      return input.hasSessionToken ? { action: 'restart', forget: 'pair' } : { action: 'login', forget: 'pair' };
  }
}

/* ---------------- route guard ---------------- */

/** boot = undecided; login / setup = that page only; ready = authenticated. */
export type AccessPhase = 'boot' | 'login' | 'setup' | 'ready';

export type GateRoute = 'connect' | 'login' | 'setup-password' | 'provision' | 'home';

const GATE_PAGES: readonly string[] = ['connect', 'login', 'setup-password'];

/** Where a navigation must go instead, or null to let it through.
 *  `online`: cached `network.status.state === 'sta_online'`; null = unknown
 *  (the status request failed), which does not seal the app. */
export function guardRedirect(routeName: string | null, phase: AccessPhase, online: boolean | null): GateRoute | null {
  const forced: GateRoute | null = phase === 'boot' ? 'connect' : phase === 'login' ? 'login' : phase === 'setup' ? 'setup-password' : null;
  if (forced) return routeName === forced ? null : forced;
  if (online === false) return routeName === 'provision' ? null : 'provision';
  // Authenticated and online: the gate pages have nothing left to do.
  return routeName !== null && GATE_PAGES.includes(routeName) ? 'home' : null;
}

/** Only in-app paths may be resumed after the gate (never a foreign URL). */
export function safeResumePath(path: unknown): string | null {
  if (typeof path !== 'string' || !path.startsWith('/') || path.startsWith('//')) return null;
  const bare = path.split(/[?#]/)[0];
  if (bare === '/' || GATE_PAGES.some((name) => bare === `/${name}`)) return null;
  return path;
}

/* ---------------- password ---------------- */

export const PASSWORD_MIN = 8;
export const PASSWORD_MAX = 64;

function utf8Length(text: string): number {
  return new TextEncoder().encode(text).length;
}

/** The device wants 8..64 characters. Counting characters for the minimum and
 *  UTF-8 bytes for the maximum is safe whichever way the device counts. */
export function validatePassword(password: string, confirm?: string): string | null {
  if (!password) return '请输入密码';
  if ([...password].length < PASSWORD_MIN) return `密码至少 ${PASSWORD_MIN} 位`;
  if (utf8Length(password) > PASSWORD_MAX) return `密码太长了，最多 ${PASSWORD_MAX} 位（一个汉字占 3 位）`;
  if (confirm !== undefined && confirm !== password) return '两次输入的密码不一致';
  return null;
}

/* ---------------- sys.auth.state / lock countdown ---------------- */

export interface AuthState {
  passwordSet: boolean;
  locked: boolean;
  retryAfterMs: number;
}

export function parseAuthState(raw: unknown): AuthState {
  const d = typeof raw === 'object' && raw !== null ? (raw as Record<string, unknown>) : {};
  const retry = retryAfterOf(d);
  return { passwordSet: d.passwordSet === true, locked: d.locked === true || retry > 0, retryAfterMs: retry };
}

/** `retryAfterMs` of an auth state or of ELOCKED error data; 0 when absent or malformed. */
export function retryAfterOf(data: unknown): number {
  const v = typeof data === 'object' && data !== null ? (data as Record<string, unknown>).retryAfterMs : undefined;
  return typeof v === 'number' && Number.isFinite(v) && v > 0 ? Math.ceil(v) : 0;
}

/** Local deadline of a lock; 0 = not locked. */
export function lockDeadline(nowMs: number, retryAfterMs: number): number {
  return retryAfterMs > 0 ? nowMs + retryAfterMs : 0;
}

/** Whole seconds left, rounded up so the form never unlocks early. */
export function lockSecondsLeft(deadlineMs: number, nowMs: number): number {
  return deadlineMs > nowMs ? Math.ceil((deadlineMs - nowMs) / 1000) : 0;
}

export function formatCountdown(seconds: number): string {
  const s = Math.max(0, Math.floor(seconds));
  return s < 60 ? `${s} 秒` : `${Math.floor(s / 60)} 分 ${String(s % 60).padStart(2, '0')} 秒`;
}
