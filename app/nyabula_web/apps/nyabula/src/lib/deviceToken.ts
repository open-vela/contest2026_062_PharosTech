/* Device access token helpers (pure). The token is a 64-hex-char secret that
 * the device prints into its provisioning QR code. Never log a token. */

const TOKEN_RE = /^[0-9a-fA-F]{64}$/;

export function isDeviceToken(value: unknown): value is string {
  return typeof value === 'string' && TOKEN_RE.test(value);
}

/** Split a route query into a valid token (or null) and the query without it. */
export function takeTokenFromQuery<T>(query: Record<string, T>): { token: string | null; present: boolean; rest: Record<string, T> } {
  const { token: raw, ...rest } = query;
  const value = Array.isArray(raw) ? raw[0] : raw;
  return { token: isDeviceToken(value) ? value : null, present: raw !== undefined, rest: rest as Record<string, T> };
}
