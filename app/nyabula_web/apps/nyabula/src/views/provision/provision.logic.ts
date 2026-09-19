/* WiFi provisioning page. Opened from the QR code the device shows on first
 * boot (http://192.168.4.1/#/provision?token=...); the router has already
 * adopted and stripped the token by the time this runs. */
import { computed, watch } from 'vue';
import { useRouter } from 'vue-router';
import { SELF_KEY, useSessionStore } from '../../stores/session';
import { useDeviceLink } from '../../composables/useDeviceLink';
import { useWifiSetup } from '../../composables/useWifiSetup';
import { NETWORK_STATE_LABEL } from '../../lib/wifi';

export function useProvisionPage() {
  const router = useRouter();
  const session = useSessionStore();
  // Device build: always the device that served the page. Hosted build: the current device.
  const key = __NYA_DEVICE__ ? SELF_KEY : (session.deviceKey ?? session.lastDeviceKey ?? SELF_KEY);
  const link = useDeviceLink(key);
  const wifi = useWifiSetup();
  const deviceBuild = __NYA_DEVICE__;

  let primed = false;
  watch(
    () => link.gate.value,
    async (gate) => {
      if (gate !== 'ready' || primed) return;
      primed = true;
      const s = await wifi.refreshStatus();
      if (s?.ssid && !wifi.ssid.value) wifi.ssid.value = s.ssid;
      if (session.isOwner) void wifi.scan();
    },
    { immediate: true },
  );

  const stateLabel = computed(() => (wifi.status.value?.state ? NETWORK_STATE_LABEL[wifi.status.value.state] : '状态未知'));
  const stateTone = computed(() => {
    const s = wifi.status.value?.state;
    return s === 'sta_online' ? 'ok' : s === 'sta_failed' ? 'err' : s === 'sta_connecting' ? 'info' : 'warn';
  });
  /** A lost link is only worth mentioning while the user is still filling the form. */
  const linkLost = computed(() => link.gate.value === 'ready' && wifi.phase.value === 'form' && !session.connected);

  function goHome(): void {
    void router.push({ name: 'home', params: { key } });
  }

  return { session, link, wifi, deviceBuild, stateLabel, stateTone, linkLost, goHome };
}

export type ProvisionPage = ReturnType<typeof useProvisionPage>;
