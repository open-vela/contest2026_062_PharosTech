/* Device name helpers (pure): the `name` / `named` pair of network.status and
 * network.name.set, and the client-side check that mirrors the device rule
 * (at most 32 UTF-8 bytes, no control characters; empty = factory default). */

export const DEVICE_NAME_MAX_BYTES = 32;

export interface DeviceNameState {
  /** Current device name; null when the firmware does not report one. */
  name: string | null;
  /** false = still the factory default name. */
  named: boolean;
}

export function utf8Length(text: string): number {
  return new TextEncoder().encode(text).length;
}

/** C0 controls and DEL (checked by code point: no control bytes in this file). */
export function hasControlChar(text: string): boolean {
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    if (c < 0x20 || c === 0x7f) return true;
  }
  return false;
}

export function parseDeviceName(raw: unknown): DeviceNameState {
  const d = typeof raw === 'object' && raw !== null ? (raw as Record<string, unknown>) : {};
  return {
    name: typeof d.name === 'string' && d.name.length > 0 ? d.name : null,
    named: d.named === true,
  };
}

/** Returns a user-facing message, or null when the name can be sent.
 *  An empty name is valid: it resets the device to its default name. */
export function validateDeviceName(name: string): string | null {
  if (hasControlChar(name)) return '名称不能包含控制字符';
  const bytes = utf8Length(name);
  if (bytes > DEVICE_NAME_MAX_BYTES) return `名称过长（${bytes}/${DEVICE_NAME_MAX_BYTES} 字节，一个汉字占 3 字节）`;
  return null;
}
