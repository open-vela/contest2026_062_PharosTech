import { computed, ref, watch } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { useSessionStore, lanKey, cloudKey, parseDeviceKey } from '../../stores/session';
import { useAccountStore } from '../../stores/account';
import { useDialogStore, useToastStore } from '@nyabula/ui';

export function useConnectPage() {
  const session = useSessionStore();
  const account = useAccountStore();
  const router = useRouter();
  const route = useRoute();
  const dialog = useDialogStore();
  const toast = useToastStore();

  const host = ref('');
  const port = ref('7788');
  const advanced = ref(false);
  const fullUrl = ref('');
  const accessToken = ref('');
  const connecting = ref(false);
  const error = ref<string | null>(null);
  const reauthKey = typeof route.query.reauth === 'string' && session.known.some(d => d.key === route.query.reauth)
    ? route.query.reauth : '';
  const reauth = !!reauthKey;
  const previous = parseDeviceKey(reauthKey);
  if (previous?.transport === 'lan') {
    if (/^wss?:\/\//.test(previous.address)) {
      advanced.value = true;
      fullUrl.value = previous.address;
    } else {
      const address = /^(.*):(\d+)$/.exec(previous.address);
      host.value = address?.[1] ?? previous.address;
      port.value = address?.[2] ?? '7788';
    }
  }

  const known = computed(() => session.known);
  const cloudDevices = computed(() => account.devices);

  function validate(): string | null {
    if (advanced.value) return /^wss?:\/\//.test(fullUrl.value.trim()) ? null : '请输入 ws:// 或 wss:// 开头的完整地址';
    if (!host.value.trim()) return '请输入设备 IP 或主机名';
    const p = Number(port.value);
    if (!Number.isInteger(p) || p < 1 || p > 65535) return '端口无效';
    return null;
  }

  async function connectLan(): Promise<void> {
    error.value = validate();
    if (error.value) return;
    const key = advanced.value ? `lan:${fullUrl.value.trim()}` : lanKey(host.value.trim(), Number(port.value));
    await open(key, accessToken.value);
    accessToken.value = '';
  }

  async function open(key: string, token?: string): Promise<void> {
    connecting.value = true;
    error.value = null;
    session.connect(key, token);
    // Wait for a terminal-ish state so the user sees feedback before nav.
    const ok = await waitState(6000);
    connecting.value = false;
    if (!ok) {
      error.value = session.lastError ?? '连接超时，请检查设备是否在线、是否同一局域网';
      toast.error(error.value);
      return;
    }
    await router.push({ name: 'home', params: { key } });
  }

  function waitState(timeoutMs: number): Promise<boolean> {
    return new Promise((resolve) => {
      const t0 = performance.now();
      const stop = watch(
        () => session.state,
        (s) => {
          if (s === 'connected' || s === 'pairing-required') {
            stop();
            resolve(true);
          } else if (s === 'closed' || (s === 'reconnecting' && performance.now() - t0 > 1500)) {
            stop();
            resolve(false);
          }
        },
        { immediate: true },
      );
      window.setTimeout(() => {
        stop();
        resolve(session.state === 'connected' || session.state === 'pairing-required');
      }, timeoutMs);
    });
  }

  async function openCloud(deviceId: string): Promise<void> {
    await open(cloudKey(deviceId));
  }

  async function forget(key: string): Promise<void> {
    if (await dialog.confirm('将删除本机保存的配对令牌，下次需重新配对。', { title: '忘记该设备？', danger: true, confirmText: '忘记' })) {
      session.forgetKnown(key);
      toast.ok('已忘记设备');
    }
  }

  return { session, account, host, port, advanced, fullUrl, accessToken, connecting, error, known, cloudDevices, connectLan, open, openCloud, forget, reauth };
}
