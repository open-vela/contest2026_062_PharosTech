/* Fail when a class name the panel uses is hidden by an ad blocker.
 *
 * EasyList's generic cosmetic rules (`##.adv-text`, `##.ad-banner`, ...) hide
 * any element carrying such a class, on every site, the device's own panel
 * included.  The element is simply gone: no error, nothing in the console,
 * and nothing wrong in a browser without a blocker, which is where the
 * panel gets developed.  The update page's advanced targets lost their whole
 * text column to `.adv-text` that way.
 *
 *   node scripts/check-adblock-classes.mjs <easylist_general_hide.txt> [more lists...]
 *
 * The lists are not vendored (they change daily and are GPL/CC licensed):
 *   https://raw.githubusercontent.com/easylist/easylist/master/easylist/easylist_general_hide.txt
 */
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const lists = process.argv.slice(2);
if (!lists.length) {
  console.error('usage: check-adblock-classes.mjs <filter list> [more lists...]');
  process.exit(2);
}

const blocked = new Set();
for (const list of lists) {
  for (const line of readFileSync(list, 'utf8').split(/\r?\n/)) {
    const match = /^##\.([A-Za-z0-9_-]+)$/.exec(line.trim());
    if (match) blocked.add(match[1]);
  }
}

const here = dirname(fileURLToPath(import.meta.url));
const roots = [resolve(here, '../src'), resolve(here, '../../../packages/ui/src')];
function* walk(dir) {
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) yield* walk(path);
    else if (/\.(vue|css|ts)$/.test(name)) yield path;
  }
}

const hits = new Map();
for (const root of roots) {
  for (const path of walk(root)) {
    const text = readFileSync(path, 'utf8');
    const used = new Set();
    for (const match of text.matchAll(/\.([A-Za-z][A-Za-z0-9_-]+)(?=[\s{,:.[>)])/g)) used.add(match[1]);
    for (const match of text.matchAll(/class="([^"]+)"/g)) for (const name of match[1].split(/\s+/)) used.add(name);
    for (const match of text.matchAll(/['"`]([A-Za-z][A-Za-z0-9_-]+)['"`]\s*:/g)) used.add(match[1]);
    for (const name of used) {
      if (!blocked.has(name)) continue;
      if (!hits.has(name)) hits.set(name, new Set());
      hits.get(name).add(relative(resolve(here, '..'), path).replaceAll('\\', '/'));
    }
  }
}

console.log(`${blocked.size} generic class rules checked`);
if (!hits.size) {
  console.log('no class name of the panel is hidden by them');
  process.exit(0);
}
for (const [name, paths] of [...hits].sort()) console.log(`.${name}  <-  ${[...paths].sort().join(', ')}`);
process.exit(1);
