/* Component registry — lets hosts extend / override NyaUI node types without
 * touching the renderer. NyaUiNode consults the registry first and falls back
 * to the built-in v1 branches, so an empty registry keeps behaviour unchanged.
 *
 * A registered component receives `node` (the NyaUiNode) as a prop and may
 * inject NYAUI_CTX for value/event plumbing. Container kinds render their own
 * children (use NyaUiNode recursively). */
import type { Component } from 'vue';

export type NyaComponentKind = 'container' | 'display' | 'input';

export interface NyaComponentDef {
  /** DSL `type` string this component renders. */
  type: string;
  kind: NyaComponentKind;
  component: Component;
}

const registry = new Map<string, NyaComponentDef>();

/** Identity helper for typed definitions (mirrors defineComponent ergonomics). */
export function defineNyaComponent(def: NyaComponentDef): NyaComponentDef {
  return def;
}

/** Register (or override) a component for a DSL type. Returns an unregister fn. */
export function registerNyaComponent(def: NyaComponentDef): () => void {
  registry.set(def.type, def);
  return () => {
    if (registry.get(def.type) === def) registry.delete(def.type);
  };
}

export function getNyaComponent(type: string): NyaComponentDef | undefined {
  return registry.get(type);
}

export function listNyaComponents(): NyaComponentDef[] {
  return [...registry.values()];
}
