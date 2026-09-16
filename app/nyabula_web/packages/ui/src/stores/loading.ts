/* Global loading state (ported from Myself).
 * boot: first paint (AppLoading: progress bar + circular clip exit).
 * route: navigation (RouteLoading: color wipe) with a readiness latch so the
 * curtain waits for the target page's first data. */
import { defineStore } from 'pinia';

export const useLoadingStore = defineStore('nyabula-loading', {
  state: () => ({
    bootProgress: 0,
    bootDone: false,
    /** Overlay still on screen (including its exit animation). */
    bootOverlayVisible: true,
    bootLabel: '正在唤醒 Nyabula',
    routeLoading: false,
    routeOverlayVisible: false,
    routePending: 0,
    routeMinUntil: 0,
    /** True once the curtain fully covers the old page. */
    routeCovered: false,
    /** Set true to disable the route curtain (e.g. shallow tab switches). */
    routeCurtainEnabled: true,
  }),
  actions: {
    setBootProgress(p: number) {
      this.bootProgress = Math.min(100, Math.max(this.bootProgress, p));
    },
    setBootLabel(label: string) {
      this.bootLabel = label;
    },
    finishBoot() {
      this.bootProgress = 100;
      this.bootDone = true;
    },
    /** Raise the curtain. Resolves once it fully covers the page, so the
     * router can swap content unseen. */
    startRoute(): Promise<void> {
      if (!this.routeCurtainEnabled) return Promise.resolve();
      if (this.routeLoading && this.routeCovered) return Promise.resolve();
      this.routeLoading = true;
      this.routeOverlayVisible = true;
      this.routeCovered = false;
      this.routePending = 0;
      return new Promise((resolve) => {
        const check = () => (this.routeCovered ? resolve() : window.setTimeout(check, 16));
        // Safety: never block navigation for more than 1s.
        window.setTimeout(resolve, 1000);
        check();
      });
    },
    /** Called by <RouteLoading> when the enter animation has finished. */
    markCovered() {
      this.routeCovered = true;
    },
    /** Pages call this while fetching; returns a release fn. */
    holdRoute(): () => void {
      if (!this.routeLoading) return () => undefined;
      this.routePending += 1;
      let released = false;
      return () => {
        if (released) return;
        released = true;
        this.routePending = Math.max(0, this.routePending - 1);
        this.tryFinishRoute();
      };
    },
    scheduleFinish(minUntil: number) {
      this.routeMinUntil = minUntil;
      window.setTimeout(() => this.tryFinishRoute(), 60);
    },
    tryFinishRoute() {
      if (!this.routeLoading) return;
      if (this.routePending > 0) return;
      if (!this.routeCovered) {
        window.setTimeout(() => this.tryFinishRoute(), 50);
        return;
      }
      const wait = this.routeMinUntil - performance.now();
      if (wait > 0) {
        window.setTimeout(() => this.tryFinishRoute(), wait);
        return;
      }
      this.routeLoading = false;
      this.routeCovered = false;
    },
  },
});
