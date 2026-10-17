/* Link between one feature and its eye scene.
 *
 * show() puts the scene on the device with the payload the feature builds;
 * while this panel holds that scene the payload is kept fresh with
 * `eyes.scene.update`. Updates are compared by value and spaced at least
 * `intervalMs` apart (1 s by default; the device interpolates numbers for
 * 320 ms between two payloads), so an unchanged scene costs no requests at all
 * and a running countdown costs one per second out of the 40/s the socket
 * allows. Nothing here polls: the payload is built from data the feature
 * already has. */
import { computed, onBeforeUnmount, watch } from 'vue';
import { useEyeStore } from '../stores/eye';
import { useSessionStore } from '../stores/session';
import type { ScenePayload } from './eyeScenePayload';

export interface EyeSceneOptions {
  /** Minimum spacing of payload updates (not below 1000). */
  intervalMs?: number;
  /** Hide the scene when the builder stops returning a payload. */
  hideWhenEmpty?: boolean;
}

export function useEyeScene(type: string, build: () => ScenePayload | null, options: EyeSceneOptions = {}) {
  const eye = useEyeStore();
  const session = useSessionStore();
  /* A little under the nominal spacing, so a value that changes exactly once
   * per interval (a seconds display) is never pushed back a whole period. */
  const gap = Math.max(1000, options.intervalMs ?? 1000) - 100;

  /** The device is showing this feature right now. */
  const shown = computed(() => eye.activeScene === type);
  /** This panel holds the scene, even if something else covers it briefly. */
  const held = computed(() => eye.webScene === type);
  const payload = computed(() => build());
  const canShow = computed(() => session.canControl && payload.value !== null);

  let sent = '';
  /* An empty builder only means "gone" once it has had something: right after
   * a reload the scene is already held while the records are still loading. */
  let hadPayload = false;
  let lastPush = -Infinity;
  let timer: ReturnType<typeof setTimeout> | undefined;
  function cancel(): void {
    if (timer) clearTimeout(timer);
    timer = undefined;
  }

  function show(): Promise<void> {
    const value = payload.value;
    if (!value) return Promise.resolve();
    // Already up and ours: a changed payload travels as an update, not a restart.
    if (held.value && shown.value) { schedule(); return Promise.resolve(); }
    cancel();
    hadPayload = true;
    sent = JSON.stringify(value);
    lastPush = performance.now();
    return eye.setScene(type, eye.sceneStyle, value);
  }
  function hide(): Promise<void> {
    cancel();
    sent = '';
    return eye.setScene(null);
  }
  function toggle(): Promise<void> {
    return shown.value || held.value ? hide() : show();
  }

  function push(): void {
    timer = undefined;
    if (!held.value || !session.canControl) return;
    const value = payload.value;
    if (!value) {
      if (options.hideWhenEmpty && hadPayload) void hide();
      return;
    }
    hadPayload = true;
    const text = JSON.stringify(value);
    if (text === sent) return;
    sent = text;
    lastPush = performance.now();
    eye.updateScene(type, value);
  }
  /** Send a changed payload now if the last one is old enough, else when it is. */
  function schedule(): void {
    if (!held.value) { cancel(); sent = ''; return; }
    if (timer) return;
    const value = payload.value;
    if (value) hadPayload = true;
    if (value ? JSON.stringify(value) === sent : !(options.hideWhenEmpty && hadPayload)) return;
    const wait = lastPush + gap - performance.now();
    if (wait <= 0) push();
    else timer = setTimeout(push, wait);
  }
  watch([held, payload], schedule, { immediate: true });
  onBeforeUnmount(cancel);

  return { shown, held, canShow, payload, show, hide, toggle };
}
