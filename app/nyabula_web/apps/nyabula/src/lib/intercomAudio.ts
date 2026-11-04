export const INTERCOM_RATE = 16000;
export const INTERCOM_MAX_SAMPLES = 1600;
export interface IntercomState {
  phase: 'off' | 'starting' | 'listening' | 'switching' | 'talking' | 'error';
  duplex: 'push-to-talk'; sampleRate: 16000; channels: 1; format: 's16le';
  capturedSamples: number; playedSamples: number; queuedPackets: number; level: number; lastError: number;
}
export function isIntercomState(value: unknown): value is IntercomState {
  const s = value as IntercomState | null;
  return !!s && ['off', 'starting', 'listening', 'switching', 'talking', 'error'].includes(s.phase) &&
    s.duplex === 'push-to-talk' && s.sampleRate === 16000 && s.channels === 1 && s.format === 's16le' &&
    Number.isFinite(s.level) && s.level >= 0 && s.level <= 32768 && Number.isFinite(s.lastError);
}
export function encodeIntercom(bytes: Uint8Array): string {
  if (!bytes.length || bytes.length > INTERCOM_MAX_SAMPLES * 2 || bytes.length % 2) throw Error('麦克风音频格式不正确');
  return btoa(String.fromCharCode(...bytes));
}
export function decodeIntercom(value: Record<string, unknown>): { sequence: number; samples: Float32Array } {
  const { sequence, samples, data } = value;
  if (typeof sequence !== 'number' || !Number.isSafeInteger(sequence) || sequence < 0 ||
      typeof samples !== 'number' || !Number.isInteger(samples) || samples < 1 || samples > INTERCOM_MAX_SAMPLES ||
      typeof data !== 'string' || data.length > 4268 || data.length % 4 || !/^[A-Za-z0-9+/]*={0,2}$/.test(data)) throw Error('设备音频数据无效');
  const raw = atob(data);
  if (raw.length !== samples * 2) throw Error('设备音频长度不一致');
  const pcm = new Float32Array(samples);
  for (let i = 0; i < samples; i++) {
    const word = raw.charCodeAt(i * 2) | raw.charCodeAt(i * 2 + 1) << 8;
    pcm[i] = (word >= 32768 ? word - 65536 : word) / 32768;
  }
  return { sequence, samples: pcm };
}

export class IntercomPlayback {
  private nodes = new Set<AudioBufferSourceNode>();
  private nextTime = 0;
  private sequence = -1;
  private closed = false;
  private constructor(private context: AudioContext) {}
  static async open() {
    const context = new AudioContext();
    try { await context.resume(); return new IntercomPlayback(context); }
    catch (error) { await context.close(); throw error; }
  }
  feed(value: Record<string, unknown>) {
    if (this.closed) return;
    const packet = decodeIntercom(value);
    if (packet.sequence <= this.sequence) return;
    if ((this.sequence >= 0 && packet.sequence !== this.sequence + 1) || this.nextTime - this.context.currentTime > .5) this.clear();
    this.sequence = packet.sequence;
    const buffer = this.context.createBuffer(1, packet.samples.length, INTERCOM_RATE);
    buffer.getChannelData(0).set(packet.samples);
    const node = this.context.createBufferSource(); node.buffer = buffer; node.connect(this.context.destination);
    const at = Math.max(this.nextTime, this.context.currentTime + .06);
    this.nextTime = at + packet.samples.length / INTERCOM_RATE;
    this.nodes.add(node); node.onended = () => { this.nodes.delete(node); node.disconnect(); };
    node.start(at);
  }
  clear() {
    for (const node of this.nodes) { node.onended = null; node.stop(); node.disconnect(); }
    this.nodes.clear(); this.nextTime = 0;
  }
  async close() { if (this.closed) return; this.closed = true; this.clear(); await this.context.close(); }
}

export class IntercomMicrophone {
  private flushed: (() => void) | null = null;
  private closed = false;
  private closing: Promise<void> | null = null;
  private constructor(private context: AudioContext, private stream: MediaStream, private source: MediaStreamAudioSourceNode,
    private worklet: AudioWorkletNode, private silent: GainNode, receive: (bytes: Uint8Array) => void) {
    worklet.port.onmessage = event => {
      if (this.closed) return;
      if (event.data?.type === 'pcm' && event.data.buffer instanceof ArrayBuffer) receive(new Uint8Array(event.data.buffer));
      else if (event.data?.type === 'flushed') { this.flushed?.(); this.flushed = null; }
    };
  }
  static async open(receive: (bytes: Uint8Array) => void) {
    if (!window.isSecureContext || !navigator.mediaDevices?.getUserMedia) throw Error('浏览器麦克风需要 HTTPS 连接');
    const stream = await navigator.mediaDevices.getUserMedia({ video: false, audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true, autoGainControl: true } });
    let context: AudioContext | undefined;
    try {
      context = new AudioContext({ sampleRate: INTERCOM_RATE });
      if (context.sampleRate !== INTERCOM_RATE || !context.audioWorklet) throw Error('当前浏览器不支持对讲采样格式');
      await context.audioWorklet.addModule(new URL('./intercomWorklet.js', import.meta.url));
      await context.resume();
      const source = context.createMediaStreamSource(stream);
      const worklet = new AudioWorkletNode(context, 'nyabula-intercom-capture', { channelCount: 1, channelCountMode: 'explicit', numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1] });
      const silent = context.createGain(); silent.gain.value = 0;
      const capture = new IntercomMicrophone(context, stream, source, worklet, silent, receive);
      source.connect(worklet); worklet.connect(silent); silent.connect(context.destination);
      return capture;
    } catch (error) { stream.getTracks().forEach(track => track.stop()); await context?.close(); throw error; }
  }
  close(flush: boolean) {
    if (!this.closing) this.closing = this.finish(flush);
    return this.closing;
  }
  private async finish(flush: boolean) {
    if (this.closed) return;
    if (flush) await new Promise<void>(resolve => {
      const timer = setTimeout(() => { this.flushed = null; resolve(); }, 200);
      this.flushed = () => { clearTimeout(timer); resolve(); }; this.worklet.port.postMessage('flush');
    });
    this.closed = true; this.worklet.port.onmessage = null;
    this.stream.getTracks().forEach(track => track.stop()); this.source.disconnect(); this.worklet.disconnect(); this.silent.disconnect();
    await this.context.close();
  }
}
