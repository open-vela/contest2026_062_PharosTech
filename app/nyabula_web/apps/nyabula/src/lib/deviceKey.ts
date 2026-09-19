/* Device-key scheme (pure, no framework imports so it is unit-testable).
 *   lan:<host>:<port>   direct LAN WebSocket (or lan:ws://... full URL)
 *   cloud:<deviceId>    Cloud relay
 *   dev:<name>          developer preview, no transport
 *   self                the device that served this page (same host and port) */

export type Transport = 'lan' | 'cloud' | 'dev' | 'self';

/** Key of the device that served the page; it carries no address. */
export const SELF_KEY = 'self';

export function parseDeviceKey(key: string): { transport: Transport; address: string } | null {
  if (key === SELF_KEY) return { transport: 'self', address: '' };
  const i = key.indexOf(':');
  if (i < 0) return null;
  const transport = key.slice(0, i);
  const address = key.slice(i + 1);
  if ((transport !== 'lan' && transport !== 'cloud' && transport !== 'dev') || !address) return null;
  return { transport, address };
}

export function lanKey(host: string, port = 7788): string {
  return `lan:${host}:${port}`;
}

export function cloudKey(deviceId: string): string {
  return `cloud:${deviceId}`;
}

/** Same-host NyaLink URL: the socket shares host and port with the page. */
export function selfWsUrl(loc: { protocol: string; host: string }): string {
  return `${loc.protocol === 'https:' ? 'wss' : 'ws'}://${loc.host}/nyalink`;
}
