/* Plugin permissions page logic shared by the three variants. */
import { computed, toRef, type Ref } from 'vue';
import { PERMISSION_META, describeError } from '@nyabula/ui';
import { useAsyncTask } from '../../composables/useRequest';
import { usePluginNav, usePluginPermissions } from './plugins.logic';

/** Short explanation per permission id (Chinese copy lives here, not in the
 * protocol table). Unknown ids fall back to a generic line. */
const PERMISSION_DESC: Record<string, string> = {
  'servo.control': '驱动舵机做动作，可能让机器人移动身体或头部。',
  'storage.private': '在插件私有目录读写文件。',
  'storage.shared.read': '读取所有插件共享的存储区。',
  'storage.shared.write': '写入共享存储区，可能影响其他插件。',
  'gpio.read': '读取通用 IO 引脚电平。',
  'gpio.write': '控制通用 IO 引脚，可驱动外接硬件。',
  'network.request': '通过 HTTP 访问互联网。',
  'network.raw_socket': '建立原始 TCP/UDP 连接，风险较高。',
  'bluetooth.scan': '扫描附近的蓝牙设备。',
  'bluetooth.connect': '连接蓝牙设备并收发数据。',
  'wifi.status': '读取 WiFi 连接状态与信号。',
  'wifi.control': '切换网络、连接或断开 WiFi。',
  'audio.play': '通过扬声器播放声音。',
  'audio.capture': '使用麦克风录音。',
  'camera.capture': '使用摄像头拍照或录像。',
  'location.read': '读取设备位置信息。',
  'ui.card': '在眼睛屏幕上显示卡片。',
  'ui.overlay': '在界面顶层绘制悬浮内容。',
  'notification.post': '向手机与面板推送通知。',
  'ai.invoke': '调用设备的 AI 能力，可能消耗额度。',
  'secrets.use': '使用你保存的密钥（不会读取明文）。',
  'background.run': '在后台持续运行。',
  'power.wake_lock': '阻止设备休眠。',
  'system.settings': '修改系统设置。',
};

export function permissionDesc(id: string): string {
  return PERMISSION_DESC[id] ?? (PERMISSION_META[id] ? '允许插件使用此系统能力。' : '插件自定义权限，请参考插件说明。');
}

/** Rough sensitivity hint for the copy column. */
export function permissionRisk(id: string): 'high' | 'mid' | 'low' {
  if (/raw_socket|camera|audio\.capture|system\.settings|secrets|gpio\.write|servo/.test(id)) return 'high';
  if (/write|control|invoke|overlay|location|bluetooth\.connect|wake_lock/.test(id)) return 'mid';
  return 'low';
}

export function usePluginPermissionsView(props: { key?: string; id: string }) {
  const id = toRef(props, 'id') as Ref<string>;
  const nav = usePluginNav(toRef(props, 'key'));
  const p = usePluginPermissions(id);

  const task = useAsyncTask(
    async () => {
      if (p.plugins.list.length === 0) await p.plugins.refresh();
      if (p.plugins.error) throw new Error(p.plugins.error);
      return true;
    },
    { holdRoute: true, immediate: true },
  );
  const errorText = computed(() => (task.error.value ? describeError(task.error.value) : null));
  const loading = computed(() => task.busy.value || p.plugins.loading);
  const notFound = computed(() => !loading.value && !errorText.value && p.plugins.list.length > 0 && !p.plugin.value);

  const disabledHint = computed(() => {
    if (!p.session.connected) return '设备未连接，无法更改权限';
    if (!p.session.isOwner) return `当前身份为「${p.session.role === 'family' ? '家人' : '访客'}」，仅设备主人可以更改权限`;
    return null;
  });

  async function grantAll() {
    for (const perm of p.perms.value) if (!perm.granted) await p.toggle(perm.name, true);
  }
  async function revokeAll() {
    for (const perm of p.perms.value) if (perm.granted) await p.toggle(perm.name, false);
  }

  return { id, ...p, ...nav, errorText, loading, notFound, disabledHint, retry: task.run, grantAll, revokeAll, permissionDesc, permissionRisk };
}
