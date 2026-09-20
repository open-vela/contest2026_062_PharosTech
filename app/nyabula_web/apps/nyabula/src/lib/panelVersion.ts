/* Which panel is this tab running, and which does the device serve now?
 *
 * The panel is a single page: a tab left open keeps running the bundle it
 * loaded, however many times the device has been given a newer one since.
 * The entry script's hashed file name is the version, so comparing the one
 * this document loaded with the one index.html names now is enough (pure;
 * fetching is the caller's business). */

const ENTRY = /assets\/(app-[A-Za-z0-9_-]+\.js)/;

/** The hashed entry script an index.html names, or null. */
export function entryOf(html: string): string | null {
  return ENTRY.exec(html)?.[1] ?? null;
}

/** The entry script this document is running, from its own script tags. */
export function runningEntry(scripts: Iterable<{ src?: string | null }>): string | null {
  for (const script of scripts) {
    const found = script.src ? ENTRY.exec(script.src)?.[1] : null;
    if (found) return found;
  }
  return null;
}

/** True only when both are known and differ: an unreadable page is not news. */
export function isStale(running: string | null, served: string | null): boolean {
  return running !== null && served !== null && running !== served;
}
