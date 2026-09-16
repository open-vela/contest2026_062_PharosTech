/* applyPatches — apply ui.patch entries (JSON Pointer paths, relative to the
 * tree document root) in place. Returns false as soon as any path cannot be
 * resolved; the caller is expected to re-fetch the full tree in that case. */
import type { NyaUiPatch, NyaUiTree } from './types.js';

function unescapeToken(token: string): string {
  return token.replace(/~1/g, '/').replace(/~0/g, '~');
}

function applyOne(doc: NyaUiTree, patch: NyaUiPatch): boolean {
  if (typeof patch?.path !== 'string' || !patch.path.startsWith('/') || patch.path.length > 1024 || /~(?:[^01]|$)/.test(patch.path)) return false;
  const tokens = patch.path.slice(1).split('/').map(unescapeToken);
  if (tokens.length === 0 || tokens.length > 16 || tokens.some(t => t === '__proto__' || t === 'constructor' || t === 'prototype')) return false;
  let cur: unknown = doc;
  for (let i = 0; i < tokens.length - 1; i++) {
    const tok = tokens[i];
    if (Array.isArray(cur)) {
      if (!/^(0|[1-9]\d*)$/.test(tok)) return false;
      const idx = Number(tok);
      if (!Number.isInteger(idx) || idx < 0 || idx >= cur.length) return false;
      cur = cur[idx];
    } else if (cur && typeof cur === 'object') {
      if (!Object.prototype.hasOwnProperty.call(cur, tok)) return false;
      cur = (cur as Record<string, unknown>)[tok];
    } else {
      return false;
    }
  }
  const last = tokens[tokens.length - 1];
  if (Array.isArray(cur)) {
    if (last === '-') {
      cur.push(patch.value);
      return true;
    }
    if (!/^(0|[1-9]\d*)$/.test(last)) return false;
    const idx = Number(last);
    if (!Number.isInteger(idx) || idx < 0 || idx >= cur.length) return false;
    cur[idx] = patch.value;
    return true;
  }
  if (cur && typeof cur === 'object') {
    (cur as Record<string, unknown>)[last] = patch.value;
    return true;
  }
  return false;
}

export function applyPatches(tree: NyaUiTree, patches: NyaUiPatch[]): boolean {
  if (!Array.isArray(patches) || patches.length > 128) return false;
  for (const p of patches ?? []) {
    if (!applyOne(tree, p)) return false;
  }
  return true;
}
