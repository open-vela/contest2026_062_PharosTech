/* Tree validation per nyaui.md limits: depth <=12, nodes <=200, strings <=4096,
 * children only under container types. Returns an error message or null. */
import { CONTAINER_TYPES, LIMITS, type NyaUiNode, type NyaUiTree } from './types.js';

function checkStrings(value: unknown): string | null {
  if (typeof value === 'string') {
    return value.length > LIMITS.maxString ? `string exceeds ${LIMITS.maxString} chars` : null;
  }
  if (Array.isArray(value)) {
    for (const v of value) {
      const err = checkStrings(v);
      if (err) return err;
    }
    return null;
  }
  if (value && typeof value === 'object') {
    for (const v of Object.values(value as Record<string, unknown>)) {
      const err = checkStrings(v);
      if (err) return err;
    }
  }
  return null;
}

export function validateTree(tree: NyaUiTree | null | undefined): string | null {
  if (!tree || typeof tree !== 'object') return 'empty tree';
  if (tree.nyaui !== 1) return `unsupported nyaui version: ${String(tree.nyaui)}`;
  if (!tree.root || typeof tree.root !== 'object') return 'missing root node';

  let count = 0;
  const walk = (node: NyaUiNode, depth: number): string | null => {
    if (!node || typeof node !== 'object' || typeof node.type !== 'string') {
      return 'malformed node';
    }
    if (depth > LIMITS.maxDepth) return `depth exceeds ${LIMITS.maxDepth}`;
    if (++count > LIMITS.maxNodes) return `node count exceeds ${LIMITS.maxNodes}`;
    const strErr = checkStrings(node.props) ?? checkStrings(node.on);
    if (strErr) return strErr;
    if (node.children && node.children.length > 0) {
      if (!CONTAINER_TYPES.has(node.type)) {
        return `type "${node.type}" may not have children`;
      }
      for (const child of node.children) {
        const err = walk(child, depth + 1);
        if (err) return err;
      }
    }
    return null;
  };
  return walk(tree.root, 1);
}
