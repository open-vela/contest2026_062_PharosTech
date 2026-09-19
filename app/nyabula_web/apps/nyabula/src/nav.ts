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

export const DEVICE_SECTIONS = [
  { id: 'overview', label: '概览', icon: 'dashboard' },
  { id: 'network', label: '网络', icon: 'wifi' },
  // Device build only: the password that lets a browser in without the QR code.
  ...(__NYA_DEVICE__ ? [{ id: 'access', label: '访问密码', icon: 'key' }] : []),
  { id: 'cloud', label: '云中继', icon: 'cloud' },
  { id: 'permissions', label: '权限总览', icon: 'shield' },
  { id: 'storage', label: '存储', icon: 'storage' },
  { id: 'update', label: '更新', icon: 'download' },
  { id: 'logs', label: '日志', icon: 'terminal' },
  { id: 'about', label: '关于', icon: 'info' },
];
