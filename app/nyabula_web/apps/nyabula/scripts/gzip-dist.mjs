/* Writes a level-9 gzip twin (<name>.gz) beside every text asset of the
 * device build. The device's static server answers with the twin when the
 * browser accepts gzip. Also enforces the device's file-name limits.
 * Usage: node scripts/gzip-dist.mjs [dir]   (default: dist-device) */
import { readdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { extname, join, relative, sep } from 'node:path';
import { constants, gzipSync } from 'node:zlib';

const root = join(import.meta.dirname, '..', process.argv[2] ?? 'dist-device');
const COMPRESS = new Set(['.html', '.js', '.css', '.svg', '.json']);
const NAME_MAX = 64;
const NAME_RE = /^[A-Za-z0-9_-][A-Za-z0-9._-]*$/;

function walk(dir) {
  return readdirSync(dir, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? walk(join(dir, e.name)) : [join(dir, e.name)]));
}
const rel = (file) => relative(root, file).split(sep).join('/');

// Drop stale twins first so a re-run never compresses or reports leftovers.
for (const file of walk(root)) if (file.endsWith('.gz')) rmSync(file);

let raw = 0;
let packed = 0;
for (const file of walk(root)) {
  const size = statSync(file).size;
  if (!COMPRESS.has(extname(file).toLowerCase())) {
    console.log(`${String(size).padStart(9)}              ${rel(file)}`);
    continue;
  }
  const gz = gzipSync(readFileSync(file), { level: constants.Z_BEST_COMPRESSION });
  writeFileSync(`${file}.gz`, gz);
  raw += size;
  packed += gz.length;
  console.log(`${String(size).padStart(9)} -> ${String(gz.length).padStart(9)} ${rel(file)}`);
}
console.log(`gzip total: ${raw} -> ${packed} bytes`);

// Every path segment, twins included, must fit the device's static server.
const bad = walk(root)
  .map(rel)
  .filter((path) => path.split('/').some((name) => name.length >= NAME_MAX || !NAME_RE.test(name)));
if (bad.length) {
  console.error(`device file-name rules violated (< ${NAME_MAX} chars, [A-Za-z0-9._-], no leading dot):`);
  for (const path of bad) console.error(`  ${path}`);
  process.exit(1);
}
