/* Pairing QR helper: builds the QR module matrix for the pairing scene.
 * Content follows Shared/protocol/nyalink.md ("配对场景"):
 *   nyabula://pair?host=<host>:<port>&code=<code>
 * with the host/port pair omitted when the payload has no host. */
import qrcode from './vendor/qrcode.js';

/* Minimal typing for the vendored (untyped) generator. */
interface QrModel {
  addData(data: string, mode?: string): void;
  make(): void;
  getModuleCount(): number;
  isDark(row: number, col: number): boolean;
}
const createQr = qrcode as unknown as (typeNumber: number, ecLevel: string) => QrModel;

export interface PairingQr {
  /** Module count per side (no quiet zone). */
  size: number;
  /** true = dark module at (row, col). */
  isDark(row: number, col: number): boolean;
  /** Encoded text, exposed for tests. */
  text: string;
}

/** Build the pairing URI. Returns null when no code is present. */
export function pairingQrText(payload: Record<string, unknown>): string | null {
  const code = typeof payload.code === 'string' && payload.code ? payload.code : null;
  if (!code) return null;
  const host = typeof payload.host === 'string' && payload.host ? payload.host : null;
  const port = typeof payload.port === 'number' ? payload.port : 7788;
  return host
    ? `nyabula://pair?host=${host}:${port}&code=${code}`
    : `nyabula://pair?code=${code}`;
}

/** Generate the QR matrix (error correction M, auto type number). */
export function makePairingQr(payload: Record<string, unknown>): PairingQr | null {
  const text = pairingQrText(payload);
  if (!text) return null;
  const qr = createQr(0, 'M'); // typeNumber 0 = auto-size
  qr.addData(text, 'Byte');
  qr.make();
  return {
    size: qr.getModuleCount(),
    isDark: (row: number, col: number) => qr.isDark(row, col),
    text,
  };
}
