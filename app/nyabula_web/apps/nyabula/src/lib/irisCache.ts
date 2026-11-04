/** Last confirmed appearance, scoped to the selected device and browser origin. */
export interface IrisPair { left: string; right: string }
const keyFor = (device: string) => `nyabula:iris:${encodeURIComponent(device)}`;
function valid(value: unknown): value is IrisPair {
  if (!value || typeof value !== 'object') return false;
  const pair = value as IrisPair;
  return typeof pair.left === 'string' && typeof pair.right === 'string' &&
    /^#[0-9a-f]{6}$/i.test(pair.left) && /^#[0-9a-f]{6}$/i.test(pair.right);
}
export function readIrisCache(device: string): IrisPair | null {
  try {
    const pair: unknown = JSON.parse(localStorage.getItem(keyFor(device)) ?? 'null');
    return valid(pair) ? pair : null;
  } catch { return null; }
}
export function rememberIris(device: string, pair: IrisPair): void {
  if (!valid(pair)) return;
  try {
    const key = keyFor(device);
    const value = JSON.stringify({ left: pair.left.toLowerCase(), right: pair.right.toLowerCase() });
    if (localStorage.getItem(key) !== value) localStorage.setItem(key, value);
  } catch { /* Private browsing may disallow storage; the live view still works. */ }
}
