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
}

export class NyaLinkError extends Error {
  constructor(
    public code: string,
    message: string,
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

  state: ConnState = 'idle';
  device: DeviceInfo | null = null;
  role: string | null = null;
  capabilities: string[] = [];
  authError: NyaLinkError | null = null;
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

  close(): void {
    this.manuallyClosed = true;
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
    await this.hello();
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
      void this.hello();
    };
    ws.onmessage = (ev) => { if (this.ws === ws) this.onMessage(String(ev.data)); };
    ws.onclose = () => {
      if (this.ws !== ws) return;
      this.clearTimers();
      this.rejectAllPending(new NyaLinkError('ECONNRESET', 'socket closed'));
      if (!this.manuallyClosed) this.scheduleReconnect();
    };
    ws.onerror = () => {
      /* onclose follows; nothing to do here */
    };
  }

  private async hello(): Promise<void> {
    this.setState('authenticating');
    try {
      const token = (await this.opts.loadToken?.(this.url)) ?? null;
      const res = (await this.request('sys.hello', {
        client: this.opts.clientKind,
        version: this.opts.version,
        token,
      })) as HelloResult;
      if (res.pairingRequired) {
        this.setState('pairing-required');
        return;
      }
      this.device = res.device ?? null;
      this.role = res.role ?? null;
      this.capabilities = Array.isArray(res.capabilities) ? res.capabilities.filter(v => typeof v === 'string') : [];
      this.authError = null;
      this.reconnectAttempt = 0;
      this.setState('connected');
      this.startPing();
    } catch (e) {
      if (e instanceof NyaLinkError && ['EACCES', 'EPERM', 'EAUTH'].includes(e.code)) {
        this.authError = e;
        this.close();
        return;
      }
      // hello failed — drop and let reconnect handle it
      this.ws?.close();
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
        p.reject(new NyaLinkError(String(d.code ?? 'EUNKNOWN'), String(d.message ?? 'error')));
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
