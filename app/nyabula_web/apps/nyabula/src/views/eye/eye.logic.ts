/* Eye page logic shared by all three variants. */
import { computed, ref } from 'vue';
import { MODES, SCENES, type EyeEngine } from '@nyabula/eye-engine';
import { useSessionStore } from '../../stores/session';
import { MODE_LABELS, SCENE_GROUPS, SCENE_META, useEyeStore } from '../../stores/eye';

export function useEyePage() {
  const session = useSessionStore();
  const eye = useEyeStore();
  const pairing = ref(false);
  const pairError = ref<string | null>(null);
  let engine: EyeEngine | null = null;

  const showPairOverlay = computed(() => session.state === 'pairing-required' || (pairing.value && session.state === 'authenticating'));
  const canvasReady = computed(() => eye.ready);

  async function doPair(code: string) {
    pairing.value = true;
    pairError.value = null;
    try {
      await session.pair(code, 'Nyabula Web');
    } catch {
      pairError.value = session.lastError && /wrong pairing code|EPERM/i.test(session.lastError) ? '配对码错误' : session.lastError ?? '配对失败';
    } finally {
      pairing.value = false;
    }
  }

  function onEngineReady(e: EyeEngine) {
    engine = e;
    if (!eye.lastState?.expression) engine.setLight(eye.lightPreview / 100);
  }
  function onLook(d: { x?: number; y?: number; release?: boolean }) {
    eye.look(d);
  }
  function onLight(v: number) {
    eye.setAmbient(v);
    // On a native Core the device applies the level and its eye.state drives
    // the preview; only a link without that topic previews locally.
    if (!eye.ambientOnDevice) engine?.setLight(v / 100);
  }

  const modes = MODES.map((m) => ({ id: m, label: MODE_LABELS[m] ?? m }));
  const sceneGroups = SCENE_GROUPS.map((g) => ({
    group: g,
    scenes: SCENES.filter((s) => (SCENE_META[s]?.group ?? '系统') === g).map((s) => ({ id: s, label: SCENE_META[s]?.label ?? s, icon: SCENE_META[s]?.icon ?? 'widgets' })),
  })).filter((g) => g.scenes.length);
  const allScenes = SCENES.map((s) => ({ id: s, label: SCENE_META[s]?.label ?? s, icon: SCENE_META[s]?.icon ?? 'widgets' }));

  const stateSummary = computed(() => {
    const s = eye.lastState;
    if (!s) return null;
    return {
      mode: s.expression?.mode,
      gaze: s.gaze?.mode,
      scene: s.scene?.type ?? null,
      style: s.scene?.style,
      light: s.expression?.lightLevel,
      seq: s.seq,
    };
  });

  return { session, eye, pairing, pairError, showPairOverlay, canvasReady, doPair, onEngineReady, onLook, onLight, modes, sceneGroups, allScenes, stateSummary };
}
