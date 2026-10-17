/* Iris colour sets and the per-device book of the owner's own sets and recent
 * colours (pure; storage is injected so the tests need no browser). */

export interface IrisPair { left: string; right: string }
export interface NamedIrisPair extends IrisPair { name: string }
export interface IrisBook { mine: NamedIrisPair[]; recent: IrisPair[] }

/** The colour the Eye Engine starts with. */
export const DEFAULT_IRIS = '#56ffb2';
export const RECENT_LIMIT = 10;

const one = (name: string, hex: string): NamedIrisPair => ({ name, left: hex, right: hex });

export const BUILTIN_IRIS_SETS: NamedIrisPair[] = [
  one('翡翠', DEFAULT_IRIS),
  one('琥珀', '#ffb347'),
  one('冰蓝', '#6fd3ff'),
  one('樱粉', '#ff8fc7'),
  one('鎏金', '#ffd24a'),
  one('紫晶', '#b08cff'),
  one('赤红', '#ff5a5a'),
  one('月白', '#e8f4ff'),
  { name: '鸳鸯', left: '#6fd3ff', right: '#ffd24a' },
  { name: '日月', left: '#ffb347', right: '#b08cff' },
  { name: '冰火', left: '#6fd3ff', right: '#ff5a5a' },
  { name: '春樱', left: DEFAULT_IRIS, right: '#ff8fc7' },
];

/** `#rrggbb` in lower case, or null for anything else. */
export function normalizeHex(value: unknown): string | null {
  if (typeof value !== 'string') return null;
  const match = /^#?([0-9a-f]{6})$/i.exec(value.trim());
  return match ? `#${match[1].toLowerCase()}` : null;
}

export const pairKey = (pair: IrisPair): string => `${pair.left}/${pair.right}`;

function cleanPair(value: unknown): IrisPair | null {
  if (typeof value !== 'object' || value === null) return null;
  const record = value as Record<string, unknown>;
  const left = normalizeHex(record.left);
  const right = normalizeHex(record.right);
  return left && right ? { left, right } : null;
}

/** Newest first, no duplicates, bounded. */
export function rememberPair(book: IrisBook, pair: IrisPair): IrisBook {
  const clean = cleanPair(pair);
  if (!clean) return book;
  return { ...book, recent: [clean, ...book.recent.filter(p => pairKey(p) !== pairKey(clean))].slice(0, RECENT_LIMIT) };
}

export function parseIrisBook(raw: string | null): IrisBook {
  const empty: IrisBook = { mine: [], recent: [] };
  if (!raw) return empty;
  try {
    const data = JSON.parse(raw) as Record<string, unknown>;
    const mine = (Array.isArray(data.mine) ? data.mine : []).flatMap(item => {
      const pair = cleanPair(item);
      const name = typeof (item as { name?: unknown })?.name === 'string' ? (item as { name: string }).name.slice(0, 12) : '';
      return pair && name ? [{ name, ...pair }] : [];
    }).slice(0, 24);
    const recent = (Array.isArray(data.recent) ? data.recent : []).flatMap(item => {
      const pair = cleanPair(item);
      return pair ? [pair] : [];
    }).slice(0, RECENT_LIMIT);
    return { mine, recent };
  } catch {
    return empty;
  }
}

const storageKey = (deviceKey: string | null | undefined): string => `nyabula.iris.${deviceKey || 'local'}`;

export function loadIrisBook(deviceKey: string | null | undefined, storage: Pick<Storage, 'getItem'> | null = globalThis.localStorage ?? null): IrisBook {
  try { return parseIrisBook(storage?.getItem(storageKey(deviceKey)) ?? null); } catch { return { mine: [], recent: [] }; }
}

export function saveIrisBook(deviceKey: string | null | undefined, book: IrisBook, storage: Pick<Storage, 'setItem'> | null = globalThis.localStorage ?? null): void {
  try { storage?.setItem(storageKey(deviceKey), JSON.stringify(book)); } catch { /* private mode: the colour still applies */ }
}
