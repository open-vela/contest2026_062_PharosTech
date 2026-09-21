/* The eye mirror stream of the device (GET /eyes/stream): records of a
 * 16-byte header and a payload of SKIP / RUN / LITERAL operations over RGB565
 * pixels, each page given as the difference to the one before it. The format
 * is described in app/nyabula/include/nyabula_eye_mirror.h. Pure: no DOM. */

export const MIRROR_HEADER = 16;
export const MIRROR_FRAME = 0;
export const MIRROR_KEEPALIVE = 1;
const FLAG_KEY = 0x01;
const OP_SKIP = 0;
const OP_RUN = 1;
const OP_LITERAL = 2;
const OP_LONG = 63;

export interface MirrorRecord {
  type: number;
  eye: number;
  key: boolean;
  scale: number;
  width: number;
  height: number;
  payload: Uint8Array;
}

/** Cuts a byte stream that arrives in arbitrary pieces into records. */
export class MirrorParser {
  private pending = new Uint8Array(0);

  push(chunk: Uint8Array): MirrorRecord[] {
    const joined = new Uint8Array(this.pending.length + chunk.length);
    joined.set(this.pending);
    joined.set(chunk, this.pending.length);
    const view = new DataView(joined.buffer);
    const records: MirrorRecord[] = [];
    let at = 0;
    while (joined.length - at >= MIRROR_HEADER) {
      if (joined[at] !== 0x4e || joined[at + 1] !== 0x45 || joined[at + 2] !== 0x4d || joined[at + 3] !== 0x31) {
        throw new Error('eye mirror: not a record');
      }
      const length = view.getUint32(at + 12, true);
      if (joined.length - at - MIRROR_HEADER < length) break;
      records.push({
        type: joined[at + 4],
        eye: joined[at + 5],
        key: (joined[at + 6] & FLAG_KEY) !== 0,
        scale: joined[at + 7],
        width: view.getUint16(at + 8, true),
        height: view.getUint16(at + 10, true),
        payload: joined.subarray(at + MIRROR_HEADER, at + MIRROR_HEADER + length),
      });
      at += MIRROR_HEADER + length;
    }
    this.pending = joined.slice(at);
    return records;
  }
}

/** One eye's page as RGBA bytes, ready for `putImageData`. */
export class MirrorPage {
  width = 0;
  height = 0;
  rgba: Uint8ClampedArray = new Uint8ClampedArray(0);

  /** Applies one frame record. A record the page cannot take (a difference
   *  to a page of another size) throws: the stream has to start over. */
  apply(record: MirrorRecord): void {
    const pixels = record.width * record.height;
    if (record.key || record.width !== this.width || record.height !== this.height) {
      if (!record.key) throw new Error('eye mirror: difference without a page');
      this.width = record.width;
      this.height = record.height;
      this.rgba = new Uint8ClampedArray(pixels * 4);
      for (let i = 3; i < this.rgba.length; i += 4) this.rgba[i] = 255;
    }
    const data = record.payload;
    const rgba = this.rgba;
    let at = 0;
    let pixel = 0;
    const put = (value: number, to: number): void => {
      const r = value >> 11;
      const g = (value >> 5) & 0x3f;
      const b = value & 0x1f;
      rgba[to] = (r << 3) | (r >> 2);
      rgba[to + 1] = (g << 2) | (g >> 4);
      rgba[to + 2] = (b << 3) | (b >> 2);
    };
    while (at < data.length) {
      const op = data[at++];
      const kind = op >> 6;
      let count = (op & 0x3f) + 1;
      if ((op & 0x3f) === OP_LONG) {
        count = data[at] | (data[at + 1] << 8);
        at += 2;
      }
      if (count === 0 || pixel + count > pixels) throw new Error('eye mirror: run past the page');
      if (kind === OP_SKIP) {
        pixel += count;
      } else if (kind === OP_RUN) {
        const value = data[at] | (data[at + 1] << 8);
        at += 2;
        for (let i = 0; i < count; i++) put(value, (pixel + i) * 4);
        pixel += count;
      } else if (kind === OP_LITERAL) {
        if (at + count * 2 > data.length) throw new Error('eye mirror: literal past the record');
        for (let i = 0; i < count; i++, at += 2) put(data[at] | (data[at + 1] << 8), (pixel + i) * 4);
        pixel += count;
      } else {
        throw new Error('eye mirror: unknown operation');
      }
    }
    if (pixel !== pixels) throw new Error('eye mirror: page not covered');
  }
}
