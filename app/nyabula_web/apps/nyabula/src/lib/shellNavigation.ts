import type { Router } from 'vue-router';

/** Browser history may contain about:blank or a different site. Only Vue
 * Router's own, renderable history entry is a valid in-app back target. */
export function hasAppBack(router: Pick<Router, 'options' | 'resolve'>, currentPath: string): boolean {
  const back = router.options.history.state.back;
  if (typeof back !== 'string' || !back.startsWith('/') || back.startsWith('//') || back === currentPath) return false;
  const target = router.resolve(back);
  return target.matched.length > 0 && !!target.name && target.meta.bare !== true &&
    !['connect', 'login', 'setup-password', 'provision'].includes(String(target.name));
}
