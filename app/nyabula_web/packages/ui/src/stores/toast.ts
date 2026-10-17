/* Toast / snackbar store. Rendered once by <ToastHost>. */
import { defineStore } from 'pinia';

export type ToastTone = 'info' | 'ok' | 'warn' | 'error';

export interface Toast {
  id: number;
  text: string;
  tone: ToastTone;
  /** Optional action button. */
  action?: { label: string; run: () => void };
  timeoutMs: number;
}

let seq = 0;

/** Map protocol error codes to user-facing Chinese copy. */
export const ERROR_COPY: Record<string, string> = {
  EPERM: '权限不足，此操作需要更高角色',
  EAUTH: '未认证或令牌失效，请重新配对',
  EINVAL: '参数无效',
  ENOTFOUND: '目标不存在',
  EBUSY: '设备正忙，请稍后重试',
  EOFFLINE: '设备离线',
  ECONFLICT: '发生冲突',
  ENETWORK: '无法连接服务器',
  ETIMEOUT: '请求超时',
};

export function describeError(e: unknown): string {
  if (e && typeof e === 'object') {
    const code = (e as { code?: string }).code;
    const msg = (e as { message?: string }).message;
    if (code && ERROR_COPY[code]) return ERROR_COPY[code] + (msg ? `（${msg}）` : '');
    if (msg) return msg;
  }
  return String(e);
}

export const useToastStore = defineStore('nyabula-toast', {
  state: () => ({ items: [] as Toast[] }),
  actions: {
    show(text: string, tone: ToastTone = 'info', opts: Partial<Pick<Toast, 'action' | 'timeoutMs'>> = {}) {
      const t: Toast = { id: ++seq, text, tone, timeoutMs: opts.timeoutMs ?? (tone === 'error' ? 6000 : 3500), action: opts.action };
      this.items.push(t);
      if (this.items.length > 4) this.items.shift();
      window.setTimeout(() => this.dismiss(t.id), t.timeoutMs);
      return t.id;
    },
    ok(text: string) {
      return this.show(text, 'ok');
    },
    warn(text: string) {
      return this.show(text, 'warn');
    },
    error(e: unknown, prefix?: string) {
      // Developer preview rejections are expected; keep the UI quiet.
      if (e && typeof e === 'object' && (e as { code?: string }).code === 'EPREVIEW') return -1;
      const text = describeError(e);
      return this.show(prefix ? `${prefix}：${text}` : text, 'error');
    },
    dismiss(id: number) {
      this.items = this.items.filter((t) => t.id !== id);
    },
  },
});
