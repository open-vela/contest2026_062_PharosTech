/* Primary navigation (5 items, identical order on every form factor). */
export interface NavItem {
  id: string;
  label: string;
  icon: string;
  /** Route name inside the device workspace, or absolute path. */
  to: string;
  /** Requires an active device session. */
  device?: boolean;
}

export const PRIMARY_NAV: NavItem[] = [
  { id: 'home', label: '主页', icon: 'home', to: 'home', device: true },
  { id: 'services', label: '功能', icon: 'widgets', to: 'services', device: true },
  { id: 'agent', label: 'Nyabot', icon: 'chat', to: 'agent', device: true },
  { id: 'plugins', label: '扩展', icon: 'extension', to: 'plugins', device: true },
  { id: 'workspace-settings', label: '设置', icon: 'settings', to: 'workspace-settings', device: true },
];

export interface DeviceSectionMeta {
  id: string;
  label: string;
  icon: string;
  /** One line under the label in directories and the settings hub. */
  desc: string;
  /** Directory group the section is listed under. */
  group: DeviceSectionGroup;
}

export type DeviceSectionGroup = 'device' | 'data' | 'system';
export const DEVICE_SECTION_GROUPS: { id: DeviceSectionGroup; label: string }[] = [
  { id: 'device', label: '设备与连接' },
  { id: 'data', label: '权限与数据' },
  { id: 'system', label: '系统维护' },
];

export const DEVICE_SECTIONS: DeviceSectionMeta[] = [
  { id: 'overview', label: '概览', icon: 'dashboard', desc: '名称、电量、连接状态', group: 'device' },
  { id: 'network', label: '网络', icon: 'wifi', desc: 'WiFi 状态与切换网络', group: 'device' },
  // Device build only: the password that lets a browser in without the QR code.
  ...(__NYA_DEVICE__ ? [{ id: 'access', label: '访问密码', icon: 'key', desc: '不扫码时进入面板的密码', group: 'device' as const }] : []),
  { id: 'cloud', label: '云中继', icon: 'cloud', desc: '外网远程访问与认领码', group: 'device' },
  { id: 'permissions', label: '权限总览', icon: 'shield', desc: '家庭成员与插件授权', group: 'data' },
  { id: 'storage', label: '存储', icon: 'storage', desc: '空间占用与 USB 存储', group: 'data' },
  { id: 'models', label: '模型', icon: 'auto_awesome', desc: '本地模型与计算域', group: 'data' },
  { id: 'update', label: '更新', icon: 'download', desc: '固件与面板版本', group: 'system' },
  { id: 'logs', label: '日志', icon: 'terminal', desc: '设备运行日志', group: 'system' },
  { id: 'about', label: '关于', icon: 'info', desc: '版本、协议与许可', group: 'system' },
];
