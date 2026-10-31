import { unzipSync, strFromU8 } from 'fflate';
export const PLUGIN_MAX_BYTES = 8 * 1024 * 1024;
export interface PluginArchive { name: string; id: string; version: string; files: Record<string, Uint8Array>; bytes: number }
export function readPluginArchive(data: Uint8Array): PluginArchive {
  if (data.length > PLUGIN_MAX_BYTES) throw new Error('压缩包不能超过 8 MiB');
  let bytes = 0;
  const names = new Set<string>();
  const files = unzipSync(data, { filter(entry) {
    if (entry.name.endsWith('/')) return false;
    if (!entry.name || entry.name.length > 240 || entry.name.split('/').some(p => !p || p === '.' || p === '..') || /[\\:\x00-\x1f]/.test(entry.name)) throw new Error('压缩包包含不安全的文件路径');
    if (names.has(entry.name)) throw new Error('压缩包包含重复文件');
    names.add(entry.name); bytes += entry.originalSize;
    if (names.size > 128 || bytes > PLUGIN_MAX_BYTES) throw new Error('解压后最多 128 个文件、8 MiB');
    return true;
  } });
  if (!files['manifest.json']) throw new Error('压缩包根目录需要 manifest.json');
  const manifest = JSON.parse(strFromU8(files['manifest.json']));
  if (typeof manifest.id !== 'string' || typeof manifest.version !== 'string') throw new Error('插件清单缺少 id 或 version');
  return { name: manifest.name || manifest.id, id: manifest.id, version: manifest.version, files, bytes };
}
export async function uploadPluginArchive(archive: PluginArchive, request: (topic: string, data: Record<string, unknown>) => Promise<any>, progress: (value: number) => void) {
  const { uploadId } = await request('plugin.upload.begin', {});
  let sent = 0;
  try {
    for (const [path, content] of Object.entries(archive.files)) {
      for (let offset = 0; offset < Math.max(1, content.length); offset += 6144) {
        const chunk = content.subarray(offset, offset + 6144);
        await request('plugin.upload.chunk', { uploadId, path, offset, data: btoa(String.fromCharCode(...chunk)) });
        sent += chunk.length; progress(Math.round(sent / Math.max(1, archive.bytes) * 95));
      }
    }
    await request('plugin.upload.commit', { uploadId }); progress(100);
  } catch (error) {
    await request('plugin.upload.cancel', { uploadId }).catch(() => {}); throw error;
  }
}
