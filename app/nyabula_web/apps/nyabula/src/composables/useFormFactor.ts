/* Form factor resolution: pointer coarseness + shortest viewport side +
 * orientation + manual override. Drives shell and page-variant selection;
 * CSS only fine-tunes inside a variant. */
import { computed, onBeforeUnmount, onMounted, reactive, readonly } from 'vue';

export type FormFactor = 'desktop' | 'tablet' | 'phone';
export type Orientation = 'landscape' | 'portrait';

const OVERRIDE_KEY = 'nyabula.formFactor';

const state = reactive({
  width: typeof window !== 'undefined' ? window.innerWidth : 1280,
  height: typeof window !== 'undefined' ? window.innerHeight : 800,
  coarse: typeof window !== 'undefined' ? window.matchMedia('(pointer: coarse)').matches : false,
  override: (typeof localStorage !== 'undefined' ? (localStorage.getItem(OVERRIDE_KEY) as FormFactor | null) : null) ?? null,
});

function measure(): void {
  state.width = window.innerWidth;
  state.height = window.innerHeight;
  state.coarse = window.matchMedia('(pointer: coarse)').matches;
}

let listeners = 0;

export function detectFormFactor(width: number, height: number, coarse: boolean): FormFactor {
  const short = Math.min(width, height);
  const long = Math.max(width, height);
  if (short < 600) return 'phone';
  // Wide + fine pointer = desktop; wide + coarse pointer stays tablet unless very large.
  if (short >= 900 && (!coarse || long >= 1600)) return 'desktop';
  if (short >= 600) return coarse ? 'tablet' : long >= 1100 ? 'desktop' : 'tablet';
  return 'tablet';
}

export function useFormFactor() {
  onMounted(() => {
    if (listeners++ === 0) {
      window.addEventListener('resize', measure, { passive: true });
      window.addEventListener('orientationchange', measure);
    }
    measure();
  });
  onBeforeUnmount(() => {
    if (--listeners === 0) {
      window.removeEventListener('resize', measure);
      window.removeEventListener('orientationchange', measure);
    }
  });

  const auto = computed<FormFactor>(() => detectFormFactor(state.width, state.height, state.coarse));
  const formFactor = computed<FormFactor>(() => state.override ?? auto.value);
  const orientation = computed<Orientation>(() => (state.width >= state.height ? 'landscape' : 'portrait'));
  const pointer = computed<'coarse' | 'fine'>(() => (state.coarse ? 'coarse' : 'fine'));

  function setOverride(v: FormFactor | null): void {
    state.override = v;
    if (v) localStorage.setItem(OVERRIDE_KEY, v);
    else localStorage.removeItem(OVERRIDE_KEY);
  }

  return {
    formFactor,
    auto,
    orientation,
    pointer,
    override: computed(() => state.override),
    setOverride,
    viewport: readonly(state),
    isDesktop: computed(() => formFactor.value === 'desktop'),
    isTablet: computed(() => formFactor.value === 'tablet'),
    isPhone: computed(() => formFactor.value === 'phone'),
  };
}
