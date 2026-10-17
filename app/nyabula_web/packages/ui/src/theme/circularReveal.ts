/*
 * View Transition API circular reveal for theme switches (ported from Myself).
 * expand: the new frame grows from (x,y) over the old one.
 * contract: the old frame shrinks into (x,y), revealing the new one.
 * Browsers without startViewTransition just apply immediately.
 */
export function circularReveal(
  origin: { x: number; y: number },
  apply: () => void,
  direction: 'expand' | 'contract' = 'expand',
): void {
  const doc = document as Document & {
    startViewTransition?: (cb: () => void) => { ready: Promise<void>; finished: Promise<void> };
  };
  const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (!doc.startViewTransition || reduced) {
    apply();
    return;
  }

  const { x, y } = origin;
  const w = window.innerWidth;
  const h = window.innerHeight;
  const radiusPx = Math.hypot(Math.max(x, w - x), Math.max(y, h - y));
  // Percent units: the view-transition pseudo box may not share the page's
  // zoomed pixel space (Chromium under page zoom / DPI scaling), but
  // percentages resolve against the box itself and always land on target.
  const px = `${((x / w) * 100).toFixed(3)}%`;
  const py = `${((y / h) * 100).toFixed(3)}%`;
  // circle() radius % resolves against hypot(w,h)/sqrt(2).
  const pr = `${((radiusPx / (Math.hypot(w, h) / Math.SQRT2)) * 100).toFixed(3)}%`;
  const root = document.documentElement;
  root.classList.toggle('vt-contract', direction === 'contract');
  root.style.setProperty('--vt-x', px);
  root.style.setProperty('--vt-y', py);

  const transition = doc.startViewTransition(apply);
  transition.ready.then(() => {
    const grow = [`circle(0% at ${px} ${py})`, `circle(${pr} at ${px} ${py})`];
    const anim = root.animate(
      { clipPath: direction === 'expand' ? grow : [...grow].reverse() },
      {
        duration: 650,
        easing: 'cubic-bezier(0.65, 0, 0.35, 1)',
        fill: 'forwards',
        pseudoElement: direction === 'expand' ? '::view-transition-new(root)' : '::view-transition-old(root)',
      },
    );
    // A forwards-filled animation lingers on the root and corrupts the next
    // transition's pseudo element; cancel only after the transition ends.
    transition.finished
      .catch(() => undefined)
      .then(() => {
        anim.cancel();
        root.classList.remove('vt-contract');
      });
  });
}

/** Center of the element that received the event, for reveal origins. */
export function eventOrigin(e: MouseEvent | PointerEvent): { x: number; y: number } {
  const el = e.currentTarget as HTMLElement | null;
  if (!el) return { x: e.clientX, y: e.clientY };
  const r = el.getBoundingClientRect();
  return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
}
