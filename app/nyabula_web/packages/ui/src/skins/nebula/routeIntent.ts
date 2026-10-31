/* Where a Nebula navigation starts and where it goes, written by the shell
 * (dock tap) or the router guard (any other navigation) and read by
 * NebulaCurtain: the lit disc blooms out of the tapped item and announces
 * the destination. x/y < 0 = unknown origin (bloom from the screen centre).
 * `at` lets the router tell a fresh dock tap from a stale one. */
import { reactive } from 'vue';

export const nebulaRouteIntent = reactive({ label: '', x: -1, y: -1, at: 0 });

export function setNebulaRouteIntent(label: string, origin?: { x: number; y: number }): void {
  nebulaRouteIntent.label = label;
  nebulaRouteIntent.x = origin?.x ?? -1;
  nebulaRouteIntent.y = origin?.y ?? -1;
  nebulaRouteIntent.at = performance.now();
}

/** True when a dock tap set the intent for the navigation now starting. */
export function isNebulaRouteIntentFresh(maxAgeMs = 400): boolean {
  return performance.now() - nebulaRouteIntent.at < maxAgeMs;
}
