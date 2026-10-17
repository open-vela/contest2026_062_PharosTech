import { defineComponent, h, inject, onBeforeUnmount, type InjectionKey, type PropType, type VNode } from 'vue';
import { NkHeader } from '@nyabula/ui';

type HeaderRenderer = () => VNode;
export const featureHeaderKey: InjectionKey<(render: HeaderRenderer) => () => void> = Symbol('feature-header');

/** Feature-specific summary/actions, rendered by the owning page shell. */
export const FeaturePageHeader = defineComponent({
  name: 'FeaturePageHeader',
  inheritAttrs: false,
  props: {
    title: { type: String, required: true },
    icon: String,
    subtitle: String,
    tone: String as PropType<'default' | 'ok' | 'warn' | 'error'>,
  },
  setup(props, { attrs, slots }) {
    const register = inject(featureHeaderKey, null);
    const render = () => h(NkHeader, { ...attrs, ...props }, slots);
    if (register) {
      const release = register(render);
      onBeforeUnmount(release);
      return () => null;
    }
    return render;
  },
});
