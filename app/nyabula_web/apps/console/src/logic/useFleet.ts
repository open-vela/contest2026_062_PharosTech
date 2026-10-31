/* Device list (fleet) data + actions: search / filter / sort, claim and
 * unclaim. Per-row clients and 30-day online rate come from the stats
 * store (current.clients and summed daily onlineSeconds). */
import { computed, ref } from 'vue';
import { useDialogStore, useToastStore } from '@nyabula/ui';
import type { Device } from '../api/client';
import { useDevicesStore } from '../stores/devices';
import { useStatsStore } from '../stores/stats';
import { useNavStore } from '../stores/nav';

export type FleetSort = 'online' | 'name' | 'coreVersion' | 'clients' | 'rate' | 'lastSeen';
export type FleetFilter = 'all' | 'on' | 'off';

export interface FleetRow extends Device {
  clients: number | null;
  rate: number | null;
}

export function useFleet() {
  const devices = useDevicesStore();
  const stats = useStatsStore();
  const nav = useNavStore();
  const toast = useToastStore();
  const dialog = useDialogStore();

  const q = ref('');
  const filter = ref<FleetFilter>('all');
  const sortKey = ref<FleetSort>('online');
  const asc = ref(false);
  const loaded = ref(false);

  async function refresh(): Promise<void> {
    await devices.refresh();
    loaded.value = true;
    await stats.loadAll(devices.devices.map((d) => d.deviceId));
  }

  const all = computed<FleetRow[]>(() =>
    devices.devices.map((d) => ({
      ...d,
      clients: stats.byId[d.deviceId]?.current.clients ?? null,
      rate: stats.onlineRate(d.deviceId),
    })),
  );
  const onlineCount = computed(() => devices.devices.filter((d) => d.online).length);

  function sortBy(k: FleetSort): void {
    if (sortKey.value === k) asc.value = !asc.value;
    else {
      sortKey.value = k;
      asc.value = k === 'name' || k === 'coreVersion';
    }
  }

  function sortValue(r: FleetRow): string | number {
    switch (sortKey.value) {
      case 'online':
        return Number(r.online);
      case 'name':
        return r.name || r.deviceId;
      case 'coreVersion':
        return r.coreVersion || '';
      case 'clients':
        return r.clients ?? -1;
      case 'rate':
        return r.rate ?? -1;
      default:
        return new Date(r.lastSeen).getTime() || 0;
    }
  }

  const rows = computed<FleetRow[]>(() => {
    const s = q.value.trim().toLowerCase();
    return all.value
      .filter((d) => filter.value === 'all' || (filter.value === 'on') === d.online)
      .filter((d) => !s || `${d.name} ${d.deviceId} ${d.coreVersion}`.toLowerCase().includes(s))
      .sort((a, b) => {
        const va = sortValue(a);
        const vb = sortValue(b);
        const r = typeof va === 'string' ? va.localeCompare(String(vb), 'zh') : va - Number(vb);
        return asc.value ? r : -r;
      });
  });

  /** POST /devices/claim. Returns true on success; errors land in
   * devices.lastError for the dialog to show inline. */
  async function claim(deviceId: string, claimCode: string): Promise<boolean> {
    const id = deviceId.trim();
    const ok = await devices.claim(id, claimCode.trim());
    if (ok) {
      toast.ok(`已认领 ${id}`);
      void stats.load(id);
    }
    return ok;
  }

  /** DELETE /devices/{id}. `confirmFirst` asks via the modal dialog (skins
   * with their own confirmation gesture pass false). */
  async function unclaim(id: string, confirmFirst = true): Promise<boolean> {
    if (confirmFirst) {
      const yes = await dialog.confirm('解绑后需重新输入认领码才能再次认领。', { title: `解绑设备 ${id}？`, danger: true, confirmText: '解绑' });
      if (!yes) return false;
    }
    const ok = await devices.unclaim(id);
    if (ok) {
      stats.forget(id);
      toast.ok(`已解绑 ${id}`);
      if (nav.deviceId === id) nav.go('devices');
    } else if (devices.lastError) {
      toast.error(devices.lastError, '解绑失败');
    }
    return ok;
  }

  return { devices, stats, nav, q, filter, sortKey, asc, sortBy, rows, all, onlineCount, loaded, refresh, claim, unclaim };
}
