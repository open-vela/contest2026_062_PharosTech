/* Feature detail frame logic: resolve the feature definition by type, load
 * its component lazily and expose the device-side status for the header. */
import { computed, defineAsyncComponent, shallowRef, watch, type Component, type Ref } from 'vue';
import { useRouter } from 'vue-router';
import { useSessionStore } from '../../stores/session';
import { MODE_LABELS, useEyeStore } from '../../stores/eye';
import { featureByType } from './features';
import type { FeatureDef } from './features/contract';

export function useServiceDetail(type: Ref<string>) {
  const session = useSessionStore();
  const eye = useEyeStore();
  const router = useRouter();

  const def = computed<FeatureDef | undefined>(() => featureByType(type.value));
  const isActive = computed(() => eye.activeScene === type.value);
  const statusText = computed(() => (isActive.value ? '正在显示' : session.connected ? '未显示' : '设备离线'));
  const tone = computed<'default' | 'ok' | 'warn'>(() => (isActive.value ? 'ok' : session.connected ? 'default' : 'warn'));

  /* Async component per type; recreated when the type changes. */
  const component = shallowRef<Component | null>(null);
  watch(
    def,
    (d) => {
      component.value = d ? defineAsyncComponent({ loader: d.load, delay: 0 }) : null;
    },
    { immediate: true },
  );

  /* Eye preview card data. */
  const modeLabel = computed(() => MODE_LABELS[eye.activeMode] ?? eye.activeMode);
  const sceneLabel = computed(() => (eye.activeScene ? featureByType(eye.activeScene)?.label ?? eye.activeScene : null));

  function back(): void {
    void router.push({ name: 'services', params: { key: session.deviceKey ?? '' } });
  }
  function exit(): Promise<void> {
    return eye.setScene(null);
  }

  return { session, eye, def, isActive, statusText, tone, component, modeLabel, sceneLabel, back, exit };
}
