/* WiFi setup shared by the provisioning page and the device network section:
 * network.status, scan + pick, credentials, submit, and the "joining" phase
 * in which the link is expected to drop while the device changes network. */
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { useToastStore } from '@nyabula/ui';
import { useSessionStore } from '../stores/session';
import { useDeviceAccessStore } from '../stores/deviceAccess';
import { isLinkDropError, isScanning, isUnknownTopic, joinFailed, normalizeScan, parseNetworkStatus, scanSeq, validateWifiInput, type NetworkStatus, type WifiNetwork } from '../lib/wifi';

/** A WiFi scan takes up to ~6 s on the device; the client default is 8 s. */
const SCAN_TIMEOUT_MS = 20000;
/** Starting a scan, or asking for its result, answers at once. */
const SCAN_START_TIMEOUT_MS = 4000;
const SCAN_POLL_MS = 1500;
/** Scan, link drop, the phone finding the hotspot again, reconnect. */
const SCAN_COLLECT_MS = 45000;
const sleep = (ms: number): Promise<void> => new Promise(resolve => setTimeout(resolve, ms));
const SET_TIMEOUT_MS = 15000;
const POLL_MS = 2500;
/** Right after the submit the device may still report its hotspot state; only
 *  trust "fell back to the hotspot" once the link bounced or this long passed. */
const JOIN_GRACE_MS = 12000;

export type WifiPhase = 'form' | 'joining' | 'joined';

export function useWifiSetup() {
  const session = useSessionStore();
  const toast = useToastStore();
  /* Device build: every status read here also feeds the cache the route
   * guard uses to keep the app sealed on /provision while offline. */
  const access = __NYA_DEVICE__ ? useDeviceAccessStore() : null;

  const status = ref<NetworkStatus | null>(null);
  const statusBusy = ref(false);
  const statusError = ref<string | null>(null);

  const networks = ref<WifiNetwork[]>([]);
  const scanning = ref(false);
  const scanned = ref(false);
  const scanError = ref<string | null>(null);

  const ssid = ref('');
  const psk = ref('');
  const formError = ref<string | null>(null);
  const submitting = ref(false);

  const phase = ref<WifiPhase>('form');
  const joinSsid = ref('');
  /** Set when the device gave up and is reachable again on its hotspot. */
  const joinError = ref<string | null>(null);
  /** The link went away after a successful submit (the expected outcome). */
  const linkDropped = ref(false);
  /** The page reaches the device through its own hotspot, which a join tears down. */
  const viaHotspot = ref(false);

  const picked = computed(() => networks.value.find((n) => n.ssid === ssid.value) ?? null);
  /** true = password required, false = open, null = unknown (typed by hand). */
  const secure = computed(() => (picked.value ? picked.value.secure : null));
  const canSubmit = computed(() => session.connected && session.isOwner && !submitting.value && !!ssid.value);

  function message(e: unknown): string {
    return e instanceof Error ? e.message : String(e);
  }

  /** `quiet`: background poll; leaves the busy flag (and its spinners) alone. */
  async function refreshStatus(quiet = false): Promise<NetworkStatus | null> {
    if (!quiet) statusBusy.value = true;
    statusError.value = null;
    try {
      status.value = parseNetworkStatus(await session.request('network.status'));
      access?.noteNetwork(status.value);
      return status.value;
    } catch (e) {
      statusError.value = message(e);
      return null;
    } finally {
      if (!quiet) statusBusy.value = false;
    }
  }

  /* The scan is started, not waited for.  On the device's own hotspot the
   * radio leaves the channel to listen on the others, the phone loses the
   * beacons and the link drops -- taking with it the answer this page used to
   * wait for.  The device keeps the list instead; it is collected here once
   * the link is back, however many requests fail in between. */
  async function scan(): Promise<void> {
    if (scanning.value) return;
    scanning.value = true;
    scanError.value = null;
    const deadline = Date.now() + SCAN_COLLECT_MS;
    try {
      let before: number | null = null;
      try {
        before = scanSeq(await session.request('network.wifi.scan', { background: true }, { timeoutMs: SCAN_START_TIMEOUT_MS }));
      } catch (e) {
        /* Firmware without the background scan refuses nothing here, it just
         * scans and answers; a dropped link is the case this is all for. */
        if (!isLinkDropError(e)) throw e;
      }
      for (;;) {
        await sleep(SCAN_POLL_MS);
        try {
          const result = await session.request('network.wifi.scan.result', {}, { timeoutMs: SCAN_START_TIMEOUT_MS });
          const seq = scanSeq(result);
          if (!isScanning(result) && (before === null ? seq > 0 : seq > before)) {
            networks.value = normalizeScan(result);
            scanned.value = true;
            return;
          }
        } catch (e) {
          if (isUnknownTopic(e)) {
            /* Older firmware: the one request that waits is all there is. */
            networks.value = normalizeScan(await session.request('network.wifi.scan', {}, { timeoutMs: SCAN_TIMEOUT_MS }));
            scanned.value = true;
            return;
          }
          /* Not connected right now: that is expected, keep asking. */
        }
        if (Date.now() > deadline) throw new Error('扫描超时，请重试');
      }
    } catch (e) {
      scanError.value = isLinkDropError(e) ? '扫描时连接中断，请稍后重试' : message(e);
    } finally {
      scanning.value = false;
    }
  }

  function pick(network: WifiNetwork): void {
    ssid.value = network.ssid;
    if (network.secure === false) psk.value = '';
    formError.value = null;
  }

  async function submit(): Promise<void> {
    if (submitting.value) return;
    const name = ssid.value;
    const pass = secure.value === false ? '' : psk.value;
    formError.value = validateWifiInput(name, pass, secure.value);
    if (formError.value) return;
    submitting.value = true;
    const before = status.value;
    viaHotspot.value = before?.state === 'ap_provision' || (!!before?.ap?.ipv4 && before.ap.ipv4 === location.hostname);
    joinError.value = null;
    linkDropped.value = false;
    try {
      const res = await session.request('network.wifi.set', { ssid: name, psk: pass }, { timeoutMs: SET_TIMEOUT_MS });
      status.value = parseNetworkStatus(res);
      access?.noteNetwork(status.value);
    } catch (e) {
      // The hotspot may go down before the answer arrives; that still means
      // the device took the credentials. Anything else is a real failure.
      if (!isLinkDropError(e)) {
        formError.value = message(e);
        submitting.value = false;
        return;
      }
      linkDropped.value = true;
    }
    psk.value = '';
    submitting.value = false;
    joinSsid.value = name;
    joinStartedAt = Date.now();
    phase.value = status.value?.state === 'sta_online' && status.value.ssid === name ? 'joined' : 'joining';
  }

  function retry(): void {
    phase.value = 'form';
    formError.value = null;
  }

  /* While joining: a dropped link is expected. If the device becomes
   * reachable again, its status tells whether it joined or fell back. */
  let pollTimer: number | undefined;
  let joinStartedAt = 0;
  async function checkJoin(): Promise<void> {
    if (phase.value !== 'joining' || !session.connected) return;
    const s = await refreshStatus();
    if (phase.value !== 'joining' || !s) return;
    if (s.state === 'sta_online') phase.value = 'joined';
    else if (joinFailed(s) && (linkDropped.value || Date.now() - joinStartedAt > JOIN_GRACE_MS)) {
      joinError.value = `设备没能连上「${joinSsid.value}」，可能是密码不对或信号太弱。`;
      phase.value = 'form';
    }
  }
  const stopPhase = watch(
    [phase, () => session.connected],
    ([p, connected]) => {
      window.clearInterval(pollTimer);
      pollTimer = undefined;
      if (p !== 'joining') return;
      if (!connected) {
        linkDropped.value = true;
        return;
      }
      if (linkDropped.value) void checkJoin();
      pollTimer = window.setInterval(() => void checkJoin(), POLL_MS);
    },
  );

  async function forget(): Promise<void> {
    try {
      status.value = parseNetworkStatus(await session.request('network.wifi.forget'));
      access?.noteNetwork(status.value);
      toast.ok('已清除保存的 WiFi');
    } catch (e) {
      if (!isLinkDropError(e)) toast.error(e, '清除 WiFi 失败');
    }
  }

  onBeforeUnmount(() => {
    stopPhase();
    window.clearInterval(pollTimer);
  });

  return {
    session, status, statusBusy, statusError, refreshStatus,
    networks, scanning, scanned, scanError, scan, pick, picked, secure,
    ssid, psk, formError, submitting, canSubmit, submit,
    phase, joinSsid, joinError, linkDropped, viaHotspot, retry, forget,
  };
}

export type WifiSetup = ReturnType<typeof useWifiSetup>;
