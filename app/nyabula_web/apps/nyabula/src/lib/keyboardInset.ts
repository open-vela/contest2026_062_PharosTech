/* Pure helper of useKeyboardInset (unit-testable, no framework imports). */

/** Pure: how much of the layout viewport the keyboard covers. */
export function keyboardInset(layoutHeight: number, visualHeight: number, visualOffsetTop: number): number {
  const inset = Math.round(layoutHeight - visualHeight - visualOffsetTop);
  // Small differences are browser chrome / zoom rounding, not a keyboard.
  return inset > 60 ? inset : 0;
}
