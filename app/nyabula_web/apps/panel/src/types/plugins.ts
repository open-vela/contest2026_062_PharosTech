/* Shapes returned by plugin.list (Shared/protocol/nyalink.md). */
export interface PluginPermission {
  name: string;
  granted: boolean;
}

export interface PluginEntry {
  id: string;
  name: string;
  version?: string;
  state?: string;
  icon?: string;
  ui?: string[];
  permissions?: PluginPermission[];
}
