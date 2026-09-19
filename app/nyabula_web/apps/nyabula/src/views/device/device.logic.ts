/* Device page logic shared by all three variants: section catalogue
 * (built-in DEVICE_SECTIONS + host settings contributions) and navigation. */
import { computed, type Component } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { DEVICE_SECTIONS } from '../../nav';
import { hostRegistry } from '../../host/registry';
import { useSessionStore } from '../../stores/session';
import OverviewSection from './sections/OverviewSection.vue';
import NetworkSection from './sections/NetworkSection.vue';
import AccessSection from './sections/AccessSection.vue';
import CloudSection from './sections/CloudSection.vue';
import PermissionsSection from './sections/PermissionsSection.vue';
import StorageSection from './sections/StorageSection.vue';
import UpdateSection from './sections/UpdateSection.vue';
import LogsSection from './sections/LogsSection.vue';
import ReservedSection from './sections/ReservedSection.vue';
import AboutSection from './sections/AboutSection.vue';

export interface DeviceSection {
  id: string;
  label: string;
  icon: string;
  component: Component;
  /** Contributed by a host extension (hostRegistry.settings). */
  extension?: boolean;
}

const BUILTIN: Record<string, Component> = {
  overview: OverviewSection,
  network: NetworkSection,
  // Listed by DEVICE_SECTIONS in the device build only.
  ...(__NYA_DEVICE__ ? { access: AccessSection } : {}),
  cloud: CloudSection,
  permissions: PermissionsSection,
  storage: StorageSection,
  update: UpdateSection,
  logs: LogsSection,
  about: AboutSection,
};

export function useDevicePage(props: { section?: string }) {
  const route = useRoute();
  const router = useRouter();
  const session = useSessionStore();

  const sections = computed<DeviceSection[]>(() => [
    ...DEVICE_SECTIONS.map((s) => ({ ...s, component: BUILTIN[s.id] ?? ReservedSection })),
    ...hostRegistry.settings().map((s) => ({ id: s.id, label: s.label, icon: s.icon ?? 'extension', component: s.component, extension: true })),
  ]);

  /** Requested section id from props/route; null when unspecified (phone shows the directory). */
  const requested = computed<string | null>(() => {
    const id = props.section ?? (typeof route.params.section === 'string' ? route.params.section : '');
    return id ? id : null;
  });
  const current = computed<DeviceSection>(() => sections.value.find((s) => s.id === (requested.value ?? 'overview')) ?? sections.value[0]!);

  function go(id: string | null): void {
    const key = typeof route.params.key === 'string' ? route.params.key : (session.deviceKey ?? '');
    void router.replace({ name: 'device', params: id ? { key, section: id } : { key } });
  }

  return { session, sections, requested, current, go };
}
