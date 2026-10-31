/* Feature list logic shared by the three variants: grouped feature tiles
 * from the registry, the currently presented feature (from eye.activeScene)
 * and open/exit actions. Consumers never see scene types or payloads. */
import { computed } from 'vue';
import { useRouter } from 'vue-router';
import { useSessionStore } from '../../stores/session';
import { useEyeStore } from '../../stores/eye';
import { FEATURES, FEATURE_GROUPS, featureByType } from './features';
import type { FeatureDef } from './features/contract';

export function useServicesPage() {
  const session = useSessionStore();
  const eye = useEyeStore();
  const router = useRouter();

  const groups = FEATURE_GROUPS.map((g) => ({ group: g, items: FEATURES.filter((f) => f.group === g) })).filter((g) => g.items.length);

  /** Definition of the feature currently shown on the device, if any. */
  const activeDef = computed<FeatureDef | null>(() => (eye.activeScene ? featureByType(eye.activeScene) ?? null : null));
  /** Live payload of the presented scene (authoritative eye.state). */
  const livePayload = computed<Record<string, unknown> | null>(() => {
    const sc = eye.lastState?.scene;
    if (!sc || !sc.type || sc.type !== eye.activeScene) return null;
    return (sc.payload as Record<string, unknown>) ?? null;
  });

  function isActive(def: FeatureDef): boolean {
    return eye.activeScene === def.type;
  }
  /** Tile subtitle: live status when running, otherwise the blurb. */
  function subFor(def: FeatureDef): string {
    const active = isActive(def);
    const s = def.status?.(active ? livePayload.value : null, active);
    return s ?? (active ? '运行中' : def.blurb);
  }
  function open(def: FeatureDef): void {
    void router.push({ name: 'service', params: { key: session.deviceKey ?? '', type: def.type } });
  }
  function exit(): Promise<void> {
    return eye.setScene(null);
  }
  /** Quick switch on the card: show this feature on the eyes, or clear it. */
  function toggle(def: FeatureDef, on: boolean): Promise<void> {
    return on ? eye.setScene(def.type) : isActive(def) ? eye.setScene(null) : Promise.resolve();
  }

  return { session, eye, groups, activeDef, livePayload, isActive, subFor, open, exit, toggle };
}
