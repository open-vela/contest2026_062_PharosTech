/*
 * Nebula theme-switch motion: a soft-edged light bloom grows from the
 * click point (expand) or the old frame dissolves into it (contract).
 * The new frame starts slightly over-exposed and settles; the old frame
 * blurs and dims underneath. Mask radius is a registered custom property
 * animated on the view-transition pseudo, so the edge stays feathered.
 * Falls back to an immediate apply without View Transitions or when the
 * user prefers reduced motion.
 */
import type { SkinTransition } from '../types';

const R = '--nb-vt-r';
let registered = false;

function registerRadius(): boolean {
  if (registered) return true;
  const css = (window as unknown as { CSS?: { registerProperty?: (d: object) => void } }).CSS;
  if (!css?.registerProperty) return false;
  try {
    css.registerProperty({ name: R, syntax: '<percentage>', inherits: false, initialValue: '0%' });
  } catch {
    /* already registered (HMR) */
  }
  registered = true;
  return true;
}

export const nebulaTransition: SkinTransition = (origin, apply, direction) => {
  const doc = document as Document & {
    startViewTransition?: (cb: () => void) => { ready: Promise<void>; finished: Promise<void> };
  };
  const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (!doc.startViewTransition || reduced || !registerRadius()) {
    apply();
    return;
  }

  const w = window.innerWidth;
  const h = window.innerHeight;
  // Percent coordinates survive page zoom / DPI scaling of the pseudo box.
  const px = `${((origin.x / w) * 100).toFixed(3)}%`;
  const py = `${((origin.y / h) * 100).toFixed(3)}%`;
  const root = document.documentElement;
  const contract = direction === 'contract';
  root.classList.add('vt-nebula');
  root.classList.toggle('vt-nebula-contract', contract);
  root.style.setProperty('--vt-x', px);
  root.style.setProperty('--vt-y', py);

  const transition = doc.startViewTransition(apply);
  transition.ready.then(() => {
    // The mask itself lives in nebula.css (var(--nb-vt-r) on the pseudo);
    // only the radius animates. Stops are % of the farthest-corner ray, so
    // 0% -> 118% always clears the viewport including the feathered edge.
    const duration = 900;
    const easing = 'cubic-bezier(0.22, 1, 0.36, 1)';
    const top = contract ? '::view-transition-old(root)' : '::view-transition-new(root)';
    const under = contract ? '::view-transition-new(root)' : '::view-transition-old(root)';
    const anims = [
      root.animate(
        { [R]: contract ? ['118%', '0%'] : ['0%', '118%'] } as PropertyIndexedKeyframes,
        { duration, easing, fill: 'forwards', pseudoElement: top },
      ),
      root.animate(
        contract
          ? { filter: ['blur(6px) brightness(0.75)', 'blur(0) brightness(1)'] }
          : { filter: ['blur(0) brightness(1)', 'blur(8px) brightness(0.7)'] },
        { duration, easing, fill: 'forwards', pseudoElement: under },
      ),
    ];
    if (!contract) {
      anims.push(
        root.animate({ filter: ['brightness(1.35) saturate(1.2)', 'brightness(1) saturate(1)'] }, { duration: duration + 150, easing, fill: 'forwards', pseudoElement: top }),
      );
    }
    // Forwards fills linger on the root; cancel only after the transition.
    transition.finished
      .catch(() => undefined)
      .then(() => {
        anims.forEach((a) => a.cancel());
        root.classList.remove('vt-nebula', 'vt-nebula-contract');
      });
  });
};
