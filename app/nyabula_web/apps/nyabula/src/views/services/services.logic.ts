/* Feature list logic shared by the three variants: the registry split into
 * what works today and what is planned, the currently presented feature
 * (from eye.activeScene) and open/exit actions. Consumers never see scene
 * types or payloads. */
import { computed } from 'vue';
import { useRouter } from 'vue-router';
import { useSessionStore } from '../../stores/session';
import { useEyeStore } from '../../stores/eye';
import { FEATURES, FEATURE_GROUPS, featureByScene } from './features';
import type { FeatureDef, FeatureStage } from './features/contract';

export interface FeatureGroup { group: string; items: FeatureDef[] }

/** Registry rows of one stage, grouped in display order; empty groups dropped. */
export function groupsOf(stage: FeatureStage, features: FeatureDef[] = FEATURES, order: string[] = FEATURE_GROUPS): FeatureGroup[] {
  return order.map((group) => ({ group, items: features.filter((f) => f.group === group && f.stage === stage) })).filter((g) => g.items.length);
}

export function useServicesPage() {
  const session = useSessionStore();
  const eye = useEyeStore();
  const router = useRouter();

  const readyGroups = groupsOf('ready');
  const plannedGroups = groupsOf('planned');
  const readyCount = readyGroups.reduce((n, g) => n + g.items.length, 0);
  const plannedCount = plannedGroups.reduce((n, g) => n + g.items.length, 0);

  /** Definition of the feature currently shown on the device, if any. */
  const activeDef = computed<FeatureDef | null>(() => (eye.activeScene ? featureByScene(eye.activeScene) ?? null : null));
  /** Live payload of the presented scene (authoritative eye.state). */
  const livePayload = computed<Record<string, unknown> | null>(() => {
    const sc = eye.lastState?.scene;
    if (!sc || !sc.type || sc.type !== eye.activeScene) return null;
    return (sc.payload as Record<string, unknown>) ?? null;
  });

  /** Sleeping is an eye expression, not a scene; everything else is a scene. */
  function isActive(def: FeatureDef): boolean {
    if (def.stage !== 'ready') return false;
    if (def.type === 'sleep') return eye.activeMode === 'sleep';
    return activeDef.value?.type === def.type;
  }
  /** Tile subtitle: live status when running, otherwise the blurb. */
  function subFor(def: FeatureDef): string {
    if (def.stage === 'planned') return def.blurb;
    const active = isActive(def);
    const s = def.status?.(active ? livePayload.value : null, active);
    return s ?? (active ? (def.type === 'sleep' ? '休眠中' : '运行中') : def.blurb);
  }
  /** Planned features have no page: nothing behind them works yet. */
  function open(def: FeatureDef): void {
    if (def.stage !== 'ready') return;
    void router.push({ name: 'service', params: { key: session.deviceKey ?? '', type: def.type } });
  }
  function exit(): Promise<void> {
    return eye.setScene(null);
  }

  return { session, eye, readyGroups, plannedGroups, readyCount, plannedCount, activeDef, livePayload, isActive, subFor, open, exit };
}
