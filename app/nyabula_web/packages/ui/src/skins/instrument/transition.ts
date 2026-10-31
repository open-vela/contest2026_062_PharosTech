/* Instrument theme-switch motion: a CRT-style scan reveal. The new frame
 * starts as a point at the click origin, snaps open into a full-width scan
 * line on that row, then steps open vertically line-block by line-block.
 * "contract" runs the same path in reverse on the old frame (a CRT power
 * off), revealing the new one beneath. Coordinates are percentages of the
 * view-transition box (see circularReveal.ts for why). */
import type { SkinTransition } from '../types';

type VtDoc = Document & {
  startViewTransition?: (cb: () => void) => { ready: Promise<void>; finished: Promise<void> };
};

export const instrumentTransition: SkinTransition = (origin, apply, direction) => {
  const doc = document as VtDoc;
  const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (!doc.startViewTransition || reduced) {
    apply();
    return;
  }

  const w = window.innerWidth;
  const h = window.innerHeight;
  const x = Math.min(100, Math.max(0, (origin.x / w) * 100));
  const y = Math.min(100, Math.max(0, (origin.y / h) * 100));
  const px = `${x.toFixed(3)}%`;
  const py = `${y.toFixed(3)}%`;
  // Half-height of the scan line, in percent of the viewport height.
  const line = ((1.5 / h) * 100).toFixed(3);

  const root = document.documentElement;
  const contract = direction === 'contract';
  root.classList.add('vt-scan');
  root.classList.toggle('vt-scan-contract', contract);
  root.style.setProperty('--vt-x', px);
  root.style.setProperty('--vt-y', py);

  const point = `inset(${py} ${(100 - x).toFixed(3)}% ${(100 - y).toFixed(3)}% ${px})`;
  const scan = `inset(calc(${py} - ${line}%) 0% calc(${(100 - y).toFixed(3)}% - ${line}%) 0%)`;
  const full = 'inset(0% 0% 0% 0%)';

  const transition = doc.startViewTransition(apply);
  transition.ready.then(() => {
    const opening: Keyframe[] = [
      { clipPath: point, offset: 0, easing: 'cubic-bezier(0.22, 1, 0.36, 1)' },
      { clipPath: scan, offset: 0.3, easing: 'steps(9, jump-end)' },
      { clipPath: full, offset: 1 },
    ];
    const closing: Keyframe[] = [
      { clipPath: full, offset: 0, easing: 'steps(9, jump-start)' },
      { clipPath: scan, offset: 0.7, easing: 'cubic-bezier(0.64, 0, 0.78, 0)' },
      { clipPath: point, offset: 1 },
    ];
    const anim = root.animate(contract ? closing : opening, {
      duration: 620,
      fill: 'forwards',
      pseudoElement: contract ? '::view-transition-old(root)' : '::view-transition-new(root)',
    });
    // Cancel only after the transition ends so the fill never leaks into
    // the next transition's pseudo elements.
    transition.finished
      .catch(() => undefined)
      .then(() => {
        anim.cancel();
        root.classList.remove('vt-scan', 'vt-scan-contract');
      });
  });
};
