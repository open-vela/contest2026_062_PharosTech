import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { MirrorPage, MirrorParser, MIRROR_FRAME, MIRROR_KEEPALIVE } from '../src/lib/eyeMirror.ts';

/* Records and pages written by the encoder of the device itself:
 * app/nyabula/tools/test_mirror_codec.c. EYE_MIRROR_DIR points at a directory
 * with full-size stream.bin / pages.bin instead of the small fixture. */
function load() {
  const dir = process.env.EYE_MIRROR_DIR;
  if (dir) {
    const [width, height] = (process.env.EYE_MIRROR_SIZE ?? '360x360').split('x').map(Number);
    return { width, height, stream: readFileSync(`${dir}/stream.bin`), pages: readFileSync(`${dir}/pages.bin`) };
  }
  const f = JSON.parse(readFileSync(new URL('./fixtures/eyeMirror.json', import.meta.url), 'utf8'));
  return { width: f.width, height: f.height, stream: Buffer.from(f.stream, 'base64'), pages: Buffer.from(f.pages, 'base64') };
}

function expected(pages, index, pixels) {
  const out = new Uint8ClampedArray(pixels * 4);
  for (let i = 0; i < pixels; i++) {
    const v = pages.readUInt16LE((index * pixels + i) * 2);
    const r = v >> 11, g = (v >> 5) & 0x3f, b = v & 0x1f;
    out.set([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255], i * 4);
  }
  return out;
}

test('every page the device encoder wrote decodes to that page', () => {
  const { width, height, stream, pages } = load();
  const pixels = width * height;
  const records = new MirrorParser().push(new Uint8Array(stream));
  assert.equal(records.length, pages.length / (pixels * 2));
  assert.ok(records.length >= 9);
  const page = new MirrorPage();
  records.forEach((record, index) => {
    assert.equal(record.type, MIRROR_FRAME);
    assert.equal(record.key, index === 0);
    page.apply(record);
    assert.deepEqual([page.width, page.height], [width, height]);
    assert.ok(Buffer.from(page.rgba.buffer).equals(Buffer.from(expected(pages, index, pixels).buffer)), `page ${index}`);
  });
});

test('the stream may arrive in pieces of any size', () => {
  const { stream } = load();
  const whole = new MirrorParser().push(new Uint8Array(stream));
  for (const step of [1, 7, 16, 17, 1000]) {
    const parser = new MirrorParser();
    const got = [];
    for (let at = 0; at < stream.length; at += step) got.push(...parser.push(new Uint8Array(stream.subarray(at, at + step))));
    assert.equal(got.length, whole.length, `step ${step}`);
    got.forEach((r, i) => assert.ok(Buffer.from(r.payload).equals(Buffer.from(whole[i].payload)), `step ${step} record ${i}`));
  }
});

test('a keepalive is a record without a payload', () => {
  const bytes = new Uint8Array(16);
  bytes.set([0x4e, 0x45, 0x4d, 0x31, MIRROR_KEEPALIVE]);
  const [record] = new MirrorParser().push(bytes);
  assert.equal(record.type, MIRROR_KEEPALIVE);
  assert.equal(record.payload.length, 0);
});

test('what is not a record, or does not fit the page, is refused', () => {
  assert.throws(() => new MirrorParser().push(new Uint8Array(16)), /not a record/);
  const { stream } = load();
  const records = new MirrorParser().push(new Uint8Array(stream));
  assert.throws(() => new MirrorPage().apply(records[1]), /difference without a page/);
  const page = new MirrorPage();
  page.apply(records[0]);
  assert.throws(() => page.apply({ ...records[1], payload: records[1].payload.subarray(0, 3) }), /eye mirror/);
  assert.throws(() => page.apply({ ...records[1], payload: new Uint8Array([0xff, 0xff, 0xff]) }), /eye mirror/);
});
