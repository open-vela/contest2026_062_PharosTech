/* Skin plumbing types shared by the registry and each skin folder. */
export type SkinTransition = (
  origin: { x: number; y: number },
  apply: () => void,
  direction: 'expand' | 'contract',
) => void;
