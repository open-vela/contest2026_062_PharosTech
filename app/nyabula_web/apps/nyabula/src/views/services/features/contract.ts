/* Feature page contract. Every feature (one per eye.scene type) is a Vue
 * component rendered inside the ServiceDetail frame. It owns its own
 * consumer-grade UI, keeps local state where the device has no topic yet,
 * and talks to the device only through useEyeStore().setScene(). */
import type { Component } from 'vue';
import type { FormFactor } from '../../../composables/useFormFactor';

export interface FeatureProps {
  /** eye.scene type, e.g. "music". */
  type: string;
  /** Current form factor; features rearrange their own grid with it. */
  ff: FormFactor;
}

/** Props for the compact control rendered inside the feature card. */
export interface FeatureMiniProps extends FeatureProps {
  /** This feature is the one currently shown on the eyes. */
  active: boolean;
  /** Live scene payload from eye.state when active, else null. */
  payload: Record<string, unknown> | null;
}

export interface FeatureDef {
  type: string;
  label: string;
  icon: string;
  group: string;
  /** Short consumer description shown on tiles. */
  blurb: string;
  load: () => Promise<{ default: Component }>;
  /** Compact in-card control (<Name>Mini.vue). */
  mini?: () => Promise<{ default: Component }>;
  /** Optional one-line live status for the tile (from current scene payload). */
  status?: (payload: Record<string, unknown> | null, active: boolean) => string | null;
}

/** Local persistence helper for feature state the device does not own yet. */
export function useFeatureMemory<T extends object>(type: string, initial: T): T {
  const key = `nyabula.feature.${type}`;
  try {
    const raw = localStorage.getItem(key);
    if (raw) return { ...initial, ...(JSON.parse(raw) as Partial<T>) };
  } catch {
    /* ignore */
  }
  return { ...initial };
}
export function saveFeatureMemory(type: string, state: object): void {
  try {
    localStorage.setItem(`nyabula.feature.${type}`, JSON.stringify(state));
  } catch {
    /* ignore */
  }
}
