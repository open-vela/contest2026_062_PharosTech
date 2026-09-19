/* Values snapshot collection + `$xxx` argument reference resolution. */
import { INPUT_TYPES, type NyaUiNode } from './types.js';

/** Walk the tree collecting every id-bearing input component's declared value,
 * then overlay local (optimistic) edits. */
export function collectValues(
  root: NyaUiNode | null | undefined,
  overrides: Record<string, unknown> = {},
): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  const walk = (node: NyaUiNode | undefined): void => {
    if (!node) return;
    if (node.id && INPUT_TYPES.has(node.type)) {
      out[node.id] = node.props?.value;
    }
    node.children?.forEach(walk);
  };
  walk(root ?? undefined);
  for (const [id, v] of Object.entries(overrides)) {
    if (id in out) out[id] = v;
  }
  return out;
}

/** Resolve action args: `{"$grams": "amount"}` -> `{grams: <value of #amount>}`.
 * Non-$ keys pass through untouched. */
export function resolveArgs(
  args: Record<string, unknown> | undefined,
  values: Record<string, unknown>,
): Record<string, unknown> | undefined {
  if (!args) return undefined;
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(args)) {
    if (k.startsWith('$')) out[k.slice(1)] = values[String(v)];
    else out[k] = v;
  }
  return out;
}
