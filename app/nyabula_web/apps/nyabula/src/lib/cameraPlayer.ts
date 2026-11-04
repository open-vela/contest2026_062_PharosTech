import { cameraCodec, type CameraSegment } from './cameraMedia';

/** Plays the existing camenc fMP4 stream; the transport is deliberately absent. */
export class CameraPlayer {
  private media: MediaSource | null = null;
  private source: SourceBuffer | null = null;
  private queue: Uint8Array[] = [];
  private queuedBytes = 0;
  private url = '';
  private epoch = 0;
  private failed = false;
  constructor(private video: HTMLVideoElement, private error: (message: string) => void) {}
  push(segment: CameraSegment) {
    if (segment.kind === 'init') {
      this.close(); this.failed = false;
      const mime = `video/mp4; codecs="${cameraCodec(segment.bytes)}"`;
      if (typeof MediaSource === 'undefined' || !MediaSource.isTypeSupported(mime)) throw Error('当前浏览器不支持这路实时画面，可拍照查看');
      const media = this.media = new MediaSource(), epoch = this.epoch;
      this.url = URL.createObjectURL(media); this.video.src = this.url;
      media.addEventListener('sourceopen', () => {
        if (epoch !== this.epoch) return;
        try {
          const source = this.source = media.addSourceBuffer(mime);
          source.mode = 'segments';
          source.addEventListener('updateend', () => { if (epoch === this.epoch) this.pump(); });
          source.addEventListener('error', () => { if (epoch === this.epoch) this.fail('画面解码失败，请重新开启'); });
          this.pump();
        } catch (e) { this.fail(e instanceof Error ? e.message : '无法打开实时画面'); }
      }, { once: true });
    }
    if (!this.media || this.failed) return;
    if (this.queuedBytes + segment.bytes.length > 2 * 1024 * 1024 || this.queue.length >= 90) {
      this.fail('播放缓冲过多，请重新开启画面'); return;
    }
    this.queue.push(segment.bytes); this.queuedBytes += segment.bytes.length;
    this.pump();
  }
  private fail(message: string) {
    if (this.failed) return;
    this.failed = true; this.close(); this.error(message);
  }
  private pump() {
    const source = this.source;
    if (this.failed || !source || source.updating || this.media?.readyState !== 'open') return;
    try {
      const buffered = source.buffered;
      if (buffered.length) {
        const start = buffered.start(0), end = buffered.end(buffered.length - 1);
        if (this.video.currentTime < start || end - this.video.currentTime > 1.2) this.video.currentTime = Math.max(start, end - .3);
        void this.video.play().catch(() => { /* The page exposes native video controls. */ });
        if (this.video.currentTime - start > 4) { source.remove(0, this.video.currentTime - 1); return; }
      }
      const bytes = this.queue.shift();
      if (bytes) { this.queuedBytes -= bytes.length; source.appendBuffer(bytes.slice().buffer); }
    } catch (e) { this.fail(e instanceof Error ? e.message : '播放画面失败'); }
  }
  close() {
    this.epoch++; this.queue = []; this.queuedBytes = 0;
    this.source = null; this.media = null;
    this.video.pause(); this.video.removeAttribute('src'); this.video.load();
    if (this.url) URL.revokeObjectURL(this.url);
    this.url = '';
  }
}
