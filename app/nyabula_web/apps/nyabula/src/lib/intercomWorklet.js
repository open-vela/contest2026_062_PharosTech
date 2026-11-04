/* Fixed 16 kHz, mono, signed little-endian PCM. No network access here. */
class NyabulaIntercomCapture extends AudioWorkletProcessor {
  constructor() {
    super();
    this.bytes = new Uint8Array(3200);
    this.used = 0;
    this.port.onmessage = event => {
      if (event.data === 'flush') {
        if (this.used) this.send();
        this.port.postMessage({ type: 'flushed' });
      }
    };
  }
  send() {
    const packet = this.bytes.slice(0, this.used);
    this.port.postMessage({ type: 'pcm', buffer: packet.buffer }, [packet.buffer]);
    this.used = 0;
  }
  process(inputs) {
    const input = inputs[0]?.[0];
    if (input) for (const sample of input) {
      const value = Math.round(Math.max(-1, Math.min(1, sample)) * (sample < 0 ? 32768 : 32767));
      this.bytes[this.used++] = value & 255;
      this.bytes[this.used++] = (value >> 8) & 255;
      if (this.used === this.bytes.length) this.send();
    }
    return true;
  }
}
registerProcessor('nyabula-intercom-capture', NyabulaIntercomCapture);
