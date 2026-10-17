/*
 * NyaLinkClient — WebSocket client SDK for NyaLink protocol v1.
 * Contract: Shared/protocol/nyalink.md (single source of truth).
 *
 * Features: auto-reconnect with exponential backoff, 5s ping / 15s pong
 * timeout, hello/pair flow, request(topic, data) -> Promise, per-topic event
 * subscription with seq regression drop, token persistence delegated to the
 * host app via callbacks.
 */

export type ConnState =
  | 'idle'
  | 'connecting'
  | 'authenticating'
  | 'pairing-required'
  | 'connected'
  | 'reconnecting'
  | 'closed';

export interface Envelope {
  v: number;
  id?: string;
  type: 'req' | 'res' | 'event' | 'err';
  topic: string;
  data: Record<string, unknown>;
}

export interface DeviceInfo {
  id: string;
  name: string;
  coreVersion: string;
}

export interface HelloResult {
  capabilities?: string[];
  device?: DeviceInfo;
  role?: 'owner' | 'family' | 'guest';
  pairingRequired?: boolean;
  /** Which credential the device accepted (password-capable devices only). */
  auth?: 'pair' | 'session';
  /** Whether an access password has been set on the device. */
  passwordSet?: boolean;
}

export class NyaLinkError extends Error {
  constructor(
    public code: string,
    message: string,
    /** Extra fields of the err frame (e.g. retryAfterMs on ELOCKED). */
    public data: Record<string, unknown> = {},
  ) {
    super(message);
    this.name = 'NyaLinkError';
  }
}

export interface NyaLinkClientOptions {
  clientKind?: 'flutter' | 'web' | 'console';
  version?: string;
  /** Return the persisted token (or null). Storage is owned by the host. */
  loadToken?: (url: string) => string | null | Promise<string | null>;
  /** Persist a freshly issued token. */
  saveToken?: (url: string, token: string) => void | Promise<void>;
  /** Do not send sys.hello when the socket opens unless a token was supplied
   *  through setToken()/authenticate(). The socket then waits (`awaitingAuth`)
   *  so the host can issue pre-auth requests (sys.auth.state, sys.login) with
   *  request() and call authenticate() itself. A socket that closes while
   *  waiting is NOT reopened: devices drop idle pre-auth sockets by design. */
  manualHello?: boolean;
  pingIntervalMs?: number; // default 5000
  pongTimeoutMs?: number; // default 15000
  reconnectBaseMs?: number; // default 500
  reconnectMaxMs?: number; // default 15000
  requestTimeoutMs?: number; // default 8000
}

export interface RequestOptions {
  /** Per-request timeout; defaults to the client's requestTimeoutMs. */
  timeoutMs?: number;
}

type EventCb = (data: Record<string, unknown>, env: Envelope) => void;
type StateCb = (state: ConnState) => void;

interface Pending {
  resolve: (data: Record<string, unknown>) => void;
  reject: (err: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

const PROTO_V = 1;

export class NyaLinkClient {
  private opts: Required<Pick<NyaLinkClientOptions, 'clientKind' | 'version' | 'pingIntervalMs' | 'pongTimeoutMs' | 'reconnectBaseMs' | 'reconnectMaxMs' | 'requestTimeoutMs'>> &
    NyaLinkClientOptions;
  private ws: WebSocket | null = null;
  private url = '';
  private seqNo = 0;
  private pending = new Map<string, Pending>();
  private listeners = new Map<string, Set<EventCb>>();
  private stateListeners = new Set<StateCb>();
  private lastSeq = new Map<string, number>();
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private lastPongAt = 0;
  private reconnectAttempt = 0;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private manuallyClosed = false;
  /** Token supplied by the host; `undefined` = none, fall back to loadToken. */
  private suppliedToken: string | null | undefined = undefined;

  state: ConnState = 'idle';
  device: DeviceInfo | null = null;
  role: string | null = null;
  capabilities: string[] = [];
  authError: NyaLinkError | null = null;
  /** Credential kind reported by the last successful hello (null if absent). */
  auth: 'pair' | 'session' | null = null;
  /** `passwordSet` reported by the last successful hello (null if absent). */
  passwordSet: boolean | null = null;
  /** manualHello only: the socket is open and no hello has been sent on it. */
  awaitingAuth = false;
  /** Estimated server clock offset (server epoch - local epoch) in ms. */
  clockOffsetMs = 0;

  constructor(options: NyaLinkClientOptions = {}) {
    this.opts = {
      clientKind: options.clientKind ?? 'web',
      version: options.version ?? '1.0.0',
      pingIntervalMs: options.pingIntervalMs ?? 5000,
      pongTimeoutMs: options.pongTimeoutMs ?? 15000,
      reconnectBaseMs: options.reconnectBaseMs ?? 500,
      reconnectMaxMs: options.reconnectMaxMs ?? 15000,
      requestTimeoutMs: options.requestTimeoutMs ?? 8000,
      ...options,
    };
  }

  /* ---------------- public API ---------------- */

  connect(url: string): void {
    this.url = url;
    this.authError = null;
    this.manuallyClosed = false;
    this.reconnectAttempt = 0;
    this.openSocket();
  }

  /** Token used by the next hello (and by every reconnect after it). Does not
   *  send anything by itself. `undefined` restores the loadToken fallback. */
  setToken(token: string | null | undefined): void {
    this.suppliedToken = token;
  }

  /** Say hello with `token` on the open socket. Resolves with the hello
   *  result; rejects with the NyaLinkError otherwise (after the usual
   *  handling: an auth rejection closes the client, anything else drops the
   *  socket). The token is kept for later reconnects. */
  async authenticate(token: string | null): Promise<HelloResult> {
    this.suppliedToken = token;
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) throw new NyaLinkError('ENOTCONN', 'socket not open');
    return this.hello(true);
  }

  close(): void {
    this.manuallyClosed = true;
    this.awaitingAuth = false;
    this.clearTimers();
    this.rejectAllPending(new NyaLinkError('ECLOSED', 'client closed'));
    this.ws?.close();
    this.ws = null;
    this.setState('closed');
  }

  /** Send a req and await its res. Rejects with NyaLinkError on err frames. */
  request(topic: string, data: Record<string, unknown> = {}, options: RequestOptions = {}): Promise<Record<string, unknown>> {
    const timeoutMs = typeof options.timeoutMs === 'number' && options.timeoutMs > 0 ? options.timeoutMs : this.opts.requestTimeoutMs;
    return new Promise((resolve, reject) => {
      if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
        reject(new NyaLinkError('ENOTCONN', 'socket not open'));
        return;
      }
      const id = `c1-${++this.seqNo}`;
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new NyaLinkError('ETIMEDOUT', `request ${topic} timed out`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this.send({ v: PROTO_V, id, type: 'req', topic, data });
    });
  }

  /** Subscribe to event frames of a topic. Returns an unsubscribe fn. */
  on(topic: string, cb: EventCb): () => void {
    let set = this.listeners.get(topic);
    if (!set) {
      set = new Set();
      this.listeners.set(topic, set);
    }
    set.add(cb);
    return () => set!.delete(cb);
  }

  onStateChange(cb: StateCb): () => void {
    this.stateListeners.add(cb);
    return () => this.stateListeners.delete(cb);
  }

  /** Complete pairing with a 6-digit code shown in the device eyes. */
  async pair(code: string, name: string): Promise<void> {
    const res = await this.request('sys.pair', { code, name });
    const token = res.token as string | undefined;
    if (!token) {
      throw new NyaLinkError('EAUTH', 'pair response missing token');
    }
    await this.opts.saveToken?.(this.url, token);
    this.role = (res.role as string) ?? 'owner';
    // Re-hello with the fresh token so the session is fully established:
    // device info + role come back and the server pushes a state snapshot.
    // Without this the client sat in a half-authorized limbo after pairing.
    await this.hello(false);
    if (this.state !== 'connected') {
      throw new NyaLinkError('EAUTH', 'post-pair hello did not connect');
    }
  }

  /* ---------------- internals ---------------- */

  private setState(s: ConnState): void {
    if (this.state === s) return;
    this.state = s;
    for (const cb of this.stateListeners) cb(s);
  }

  private openSocket(): void {
    this.clearTimers();
    this.setState(this.reconnectAttempt > 0 ? 'reconnecting' : 'connecting');
    let ws: WebSocket;
    try {
      ws = new WebSocket(this.url);
    } catch (e) {
      this.scheduleReconnect();
      return;
    }
    this.ws = ws;
    ws.onopen = () => {
      if (this.ws !== ws) return;
      // Fresh connection = fresh server-side seq domain. Keeping the old
      // per-topic seq watermarks silently dropped every event after a
      // device restart (new seq starts below the stale watermark), which
      // froze the eye canvas even though the link looked healthy.
      this.lastSeq.clear();
      if (this.opts.manualHello && this.suppliedToken === undefined) {
        // Wait for the host: pre-auth requests first, then authenticate().
        this.awaitingAuth = true;
        this.setState('authenticating');
        return;
      }
      void this.hello(false);
    };
    ws.onmessage = (ev) => { if (this.ws === ws) this.onMessage(String(ev.data)); };
    ws.onclose = () => {
      if (this.ws !== ws) return;
      this.clearTimers();
      this.rejectAllPending(new NyaLinkError('ECONNRESET', 'socket closed'));
      if (this.awaitingAuth) {
        // An idle pre-auth socket timing out is normal; the host reopens
        // one when it needs it, so never hold or churn sockets here.
        this.awaitingAuth = false;
        this.manuallyClosed = true;
        this.ws = null;
        this.setState('closed');
        return;
      }
      if (!this.manuallyClosed) this.scheduleReconnect();
    };
    ws.onerror = () => {
      /* onclose follows; nothing to do here */
    };
  }

  private async hello(rethrow: true): Promise<HelloResult>;
  private async hello(rethrow: false): Promise<HelloResult | null>;
  private async hello(rethrow: boolean): Promise<HelloResult | null> {
    this.awaitingAuth = false;
    this.setState('authenticating');
    try {
      const token = this.suppliedToken !== undefined ? this.suppliedToken : ((await this.opts.loadToken?.(this.url)) ?? null);
      const res = (await this.request('sys.hello', {
        client: this.opts.clientKind,
        version: this.opts.version,
        token,
      })) as HelloResult;
      if (res.pairingRequired) {
        this.setState('pairing-required');
        return res;
      }
      this.device = res.device ?? null;
      this.role = res.role ?? null;
      this.capabilities = Array.isArray(res.capabilities) ? res.capabilities.filter(v => typeof v === 'string') : [];
      this.auth = res.auth === 'pair' || res.auth === 'session' ? res.auth : null;
      this.passwordSet = typeof res.passwordSet === 'boolean' ? res.passwordSet : null;
      this.authError = null;
      this.reconnectAttempt = 0;
      this.setState('connected');
      this.startPing();
      return res;
    } catch (e) {
      if (e instanceof NyaLinkError && ['EACCES', 'EPERM', 'EAUTH'].includes(e.code)) {
        this.authError = e;
        this.close();
      } else {
        // hello failed — drop and let reconnect handle it
        this.ws?.close();
      }
      if (rethrow) throw e;
      return null;
    }
  }

  private startPing(): void {
    this.lastPongAt = Date.now();
    if (this.pingTimer) clearInterval(this.pingTimer);
    this.pingTimer = setInterval(() => {
      if (Date.now() - this.lastPongAt > this.opts.pongTimeoutMs) {
        // Dead link: force close; onclose schedules reconnect.
        this.ws?.close();
        return;
      }
      const sentAt = Date.now();
      this.request('sys.ping', {})
        .then((res) => {
          this.lastPongAt = Date.now();
          const t = res.t as number | undefined;
          if (typeof t === 'number') {
            const rtt = Date.now() - sentAt;
            this.clockOffsetMs = t + rtt / 2 - Date.now();
          }
        })
        .catch(() => {
          /* timeout handled by pongTimeout check */
        });
    }, this.opts.pingIntervalMs);
  }

  private scheduleReconnect(): void {
    if (this.manuallyClosed || this.reconnectTimer) return;
    const delay = Math.min(
      this.opts.reconnectMaxMs,
      this.opts.reconnectBaseMs * Math.pow(2, this.reconnectAttempt),
    );
    this.reconnectAttempt++;
    this.setState('reconnecting');
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.openSocket();
    }, delay);
  }

  private clearTimers(): void {
    if (this.pingTimer) {
      clearInterval(this.pingTimer);
      this.pingTimer = null;
    }
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
  }

  private rejectAllPending(err: Error): void {
    for (const [, p] of this.pending) {
      clearTimeout(p.timer);
      p.reject(err);
    }
    this.pending.clear();
  }

  private send(env: Envelope): void {
    this.ws?.send(JSON.stringify(env));
  }

  private onMessage(raw: string): void {
    let env: Envelope;
    try {
      env = JSON.parse(raw) as Envelope;
    } catch {
      return; // tolerate garbage per protocol leniency
    }
    if (!env || typeof env !== 'object') return;
    if (env.type === 'res' || (env.type === 'err' && env.id)) {
      const p = env.id ? this.pending.get(env.id) : undefined;
      if (!p) return;
      this.pending.delete(env.id!);
      clearTimeout(p.timer);
      if (env.type === 'res') p.resolve(env.data ?? {});
      else {
        const d = env.data ?? {};
        p.reject(new NyaLinkError(String(d.code ?? 'EUNKNOWN'), String(d.message ?? 'error'), d));
      }
      return;
    }
    if (env.type === 'event') {
      const seq = env.data?.seq;
      if (typeof seq === 'number') {
        const last = this.lastSeq.get(env.topic);
        if (last !== undefined && seq <= last) return; // drop out-of-order
        this.lastSeq.set(env.topic, seq);
      }
      const set = this.listeners.get(env.topic);
      if (set) for (const cb of set) cb(env.data ?? {}, env);
    }
    // Unsolicited err frames without id are logged and otherwise ignored.
  }
}
