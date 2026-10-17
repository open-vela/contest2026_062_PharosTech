/* Account page logic shared by all three variants: auth form, overview
 * stats, device list actions and claim form. */
import { computed, ref } from 'vue';
import { useRouter } from 'vue-router';
import { useDialogStore, useToastStore } from '@nyabula/ui';
import { useAccountStore } from '../../stores/account';
import { cloudKey } from '../../stores/session';
import { useAsyncTask } from '../../composables/useRequest';

export type AccountSection = 'overview' | 'devices' | 'claim' | 'market';

export const ACCOUNT_SECTIONS: { id: AccountSection; label: string; icon: string }[] = [
  { id: 'overview', label: '概览', icon: 'dashboard' },
  { id: 'devices', label: '设备', icon: 'devices' },
  { id: 'claim', label: '认领', icon: 'add' },
  { id: 'market', label: '市场', icon: 'store' },
];

export function useAccountPage() {
  const account = useAccountStore();
  const toast = useToastStore();
  const dialog = useDialogStore();
  const router = useRouter();

  /* ---- auth ---- */
  const authMode = ref<'login' | 'register'>('login');
  const authTabs = [
    { id: 'login', label: '登录', icon: 'login' },
    { id: 'register', label: '注册', icon: 'person' },
  ];
  const email = ref('');
  const password = ref('');
  const name = ref('');
  const authBusy = ref(false);
  const authError = ref<string | null>(null);

  const authValid = computed(() => {
    if (!email.value.trim() || password.value.length < 6) return false;
    if (authMode.value === 'register' && !name.value.trim()) return false;
    return true;
  });

  async function submitAuth(): Promise<void> {
    if (!authValid.value || authBusy.value) return;
    authBusy.value = true;
    authError.value = null;
    try {
      if (authMode.value === 'login') await account.login(email.value.trim(), password.value);
      else await account.register(email.value.trim(), password.value, name.value.trim());
      toast.ok(authMode.value === 'login' ? '登录成功' : '注册成功');
      password.value = '';
      await loader.run();
    } catch (e) {
      authError.value = e instanceof Error ? e.message : String(e);
      toast.error(e, authMode.value === 'login' ? '登录失败' : '注册失败');
    } finally {
      authBusy.value = false;
    }
  }

  /* ---- data ---- */
  async function loadAll(): Promise<void> {
    if (!account.loggedIn) return;
    await Promise.all([account.refreshDevices(), account.refreshOverview()]);
  }
  const loader = useAsyncTask(loadAll, { errorPrefix: '加载账号数据失败', holdRoute: true, immediate: true });

  const statCards = computed(() => {
    const o = account.overview;
    return [
      { id: 'devices', label: '设备', value: o?.devices ?? 0, icon: 'devices' },
      { id: 'online', label: '在线', value: o?.online ?? 0, icon: 'wifi' },
      { id: 'clients', label: '客户端', value: o?.clients ?? 0, icon: 'apps' },
      { id: 'frames', label: '今日帧', value: o?.framesToday ?? 0, icon: 'bolt' },
    ];
  });

  /* ---- device actions ---- */
  function openRemote(deviceId: string): void {
    void router.push({ name: 'home', params: { key: cloudKey(deviceId) } });
  }
  function openStats(deviceId: string): void {
    void router.push({ name: 'account-device', params: { id: deviceId } });
  }
  async function unclaim(deviceId: string, label?: string): Promise<void> {
    const ok = await dialog.confirm(`解绑后需重新输入认领码才能再次绑定「${label || deviceId}」。`, {
      title: '解绑设备',
      danger: true,
      confirmText: '解绑',
    });
    if (!ok) return;
    try {
      await account.unclaim(deviceId);
      toast.ok('已解绑');
      await account.refreshOverview().catch(() => undefined);
    } catch (e) {
      toast.error(e, '解绑失败');
    }
  }

  /* ---- claim ---- */
  const claimId = ref('');
  const claimCode = ref('');
  const claimBusy = ref(false);
  const claimValid = computed(() => !!claimId.value.trim() && !!claimCode.value.trim());
  async function submitClaim(): Promise<boolean> {
    if (!claimValid.value || claimBusy.value) return false;
    claimBusy.value = true;
    try {
      const d = await account.claim(claimId.value.trim(), claimCode.value.trim());
      toast.ok(`已认领 ${d.name || d.deviceId}`);
      claimId.value = '';
      claimCode.value = '';
      await account.refreshOverview().catch(() => undefined);
      return true;
    } catch (e) {
      toast.error(e, '认领失败');
      return false;
    } finally {
      claimBusy.value = false;
    }
  }

  async function logout(): Promise<void> {
    const ok = await dialog.confirm('退出后需重新登录才能使用 Cloud 功能。', { title: '退出登录', confirmText: '退出' });
    if (!ok) return;
    account.logout();
    toast.ok('已退出登录');
  }

  function formatTime(iso: string): string {
    if (!iso) return '—';
    const d = new Date(iso);
    return Number.isNaN(d.getTime()) ? iso : d.toLocaleString();
  }

  return {
    account,
    authMode,
    authTabs,
    email,
    password,
    name,
    authBusy,
    authError,
    authValid,
    submitAuth,
    loader,
    reload: loader.run,
    statCards,
    openRemote,
    openStats,
    unclaim,
    claimId,
    claimCode,
    claimBusy,
    claimValid,
    submitClaim,
    logout,
    formatTime,
  };
}
