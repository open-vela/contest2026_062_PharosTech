/* Global modal dialog store: replaces native alert/confirm/prompt.
 * Rendered once by <AppModal>. Promise-based API for callers. */
import { defineStore } from 'pinia';

export interface DialogOptions {
  title?: string;
  message?: string;
  confirmText?: string;
  cancelText?: string;
  /** Destructive styling for the confirm button. */
  danger?: boolean;
  /** Prompt mode: show a text field with this initial value. */
  input?: { value?: string; placeholder?: string; label?: string };
}

interface ActiveDialog extends DialogOptions {
  id: number;
  kind: 'alert' | 'confirm' | 'prompt';
  resolve: (v: unknown) => void;
}

let seq = 0;

export const useDialogStore = defineStore('nyabula-dialog', {
  state: () => ({
    active: null as ActiveDialog | null,
    queue: [] as ActiveDialog[],
    /** Present the modal as a bottom sheet (phone form factor). */
    sheetMode: false,
  }),
  actions: {
    push(d: ActiveDialog) {
      if (this.active) this.queue.push(d);
      else this.active = d;
    },
    alert(message: string, opts: DialogOptions = {}): Promise<void> {
      return new Promise((resolve) => {
        this.push({ id: ++seq, kind: 'alert', message, ...opts, resolve: () => resolve() });
      });
    },
    confirm(message: string, opts: DialogOptions = {}): Promise<boolean> {
      return new Promise((resolve) => {
        this.push({ id: ++seq, kind: 'confirm', message, ...opts, resolve: (v) => resolve(Boolean(v)) });
      });
    },
    prompt(message: string, opts: DialogOptions = {}): Promise<string | null> {
      return new Promise((resolve) => {
        this.push({
          id: ++seq,
          kind: 'prompt',
          message,
          input: { value: '', ...(opts.input ?? {}) },
          ...opts,
          resolve: (v) => resolve(v === null ? null : String(v)),
        });
      });
    },
    /** Called by <AppModal> when the user acts. */
    settle(value: unknown) {
      const d = this.active;
      if (!d) return;
      this.active = this.queue.shift() ?? null;
      d.resolve(value);
    },
  },
});
