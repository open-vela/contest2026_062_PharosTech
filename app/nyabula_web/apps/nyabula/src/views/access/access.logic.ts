/* Device build only: logic of the two password gate pages, shared by their
 * phone and desktop variants. Never log a token or a password. */
import { computed, inject, onBeforeUnmount, onMounted, ref } from 'vue';
import { useRouter } from 'vue-router';
import { useSessionStore } from '../../stores/session';
import { errorCode, useDeviceAccessStore } from '../../stores/deviceAccess';
import type { useFormFactor } from '../../composables/useFormFactor';
import { formatCountdown, lockSecondsLeft, validatePassword } from '../../lib/deviceAccess';

const LINK_TROUBLE = '连接不上设备，请确认手机或电脑和设备在同一个网络里，然后再试一次。';

/** Map a password-change failure to a message; shared with the settings section. */
export function passwordChangeError(e: unknown, authMessage: string): string {
  switch (errorCode(e)) {
    case 'EWEAK':
      return '设备没有接受这个密码：长度需要在 8 到 64 位之间。';
    case 'EAUTH':
      return authMessage;
    case 'EPROTO':
      return e instanceof Error ? e.message : '设备返回的结果无效';
    default:
      return '没有改成：与设备的连接中断了，请稍后再试一次。';
  }
}

export function useLoginPage() {
  const router = useRouter();
  const access = useDeviceAccessStore();
  const ff = inject<ReturnType<typeof useFormFactor>>('formFactor');

  const password = ref('');
  const show = ref(false);
  const busy = ref(false);
  const checking = ref(false);
  const error = ref<string | null>(null);
  /** Bumped after a wrong password so the variant can refocus the field. */
  const refocus = ref(0);

  /* Lock countdown: the deadline lives in the store, the clock ticks here. */
  const now = ref(Date.now());
  let ticker: number | undefined;
  const lockLeft = computed(() => lockSecondsLeft(access.lockUntil, now.value));
  const locked = computed(() => lockLeft.value > 0);
  const lockText = computed(() => formatCountdown(lockLeft.value));

  /** No password on the device yet: logging in cannot work, the owner must scan the code first. */
  const noPassword = computed(() => access.authState?.passwordSet === false);
  /** A computer cannot scan a code: say to do the first step from a phone. */
  const onComputer = computed(() => ff?.formFactor.value !== 'phone');
  const canSubmit = computed(() => !busy.value && !checking.value && !locked.value && !noPassword.value && password.value.length > 0);

  /** Reads sys.auth.state on a short-lived socket; the page holds none while idle. */
  async function check(force = false): Promise<void> {
    checking.value = true;
    try {
      await access.ensureAuthState(force);
      if (error.value === LINK_TROUBLE) error.value = null;
    } catch {
      error.value = LINK_TROUBLE;
    } finally {
      checking.value = false;
    }
  }

  async function submit(): Promise<void> {
    if (!canSubmit.value) return;
    busy.value = true;
    error.value = null;
    try {
      await access.login(password.value);
      password.value = '';
      await router.replace(access.nextLocation());
    } catch (e) {
      const code = errorCode(e);
      now.value = Date.now();
      if (code === 'EAUTH') {
        error.value = '密码不对，请再试一次。';
        password.value = '';
        refocus.value++;
      } else if (code === 'ELOCKED' || code === 'ENOPASSWORD') {
        error.value = null; // the lock notice / the setup explanation take over
        password.value = '';
      } else if (code === 'EPROTO') {
        error.value = e instanceof Error ? e.message : LINK_TROUBLE;
      } else {
        error.value = LINK_TROUBLE;
      }
    } finally {
      busy.value = false;
    }
  }

  onMounted(() => {
    ticker = window.setInterval(() => (now.value = Date.now()), 500);
    void check();
  });
  onBeforeUnmount(() => window.clearInterval(ticker));

  return { password, show, busy, checking, error, refocus, locked, lockText, noPassword, onComputer, canSubmit, check, submit };
}

export type LoginPage = ReturnType<typeof useLoginPage>;

export function useSetupPasswordPage() {
  const router = useRouter();
  const session = useSessionStore();
  const access = useDeviceAccessStore();

  const password = ref('');
  const confirm = ref('');
  const show = ref(false);
  const busy = ref(false);
  const error = ref<string | null>(null);

  /** reset = the device already has a password; the scanned code allows replacing it. */
  const reset = computed(() => access.setupMode === 'reset');
  const canSubmit = computed(() => !busy.value && session.connected && password.value.length > 0 && confirm.value.length > 0);
  const title = computed(() => (reset.value ? '重新设置访问密码' : '先设置一个访问密码'));
  const sub = computed(() => (reset.value ? '你是扫描设备上的二维码进来的，可以直接换一个新密码。' : '有了密码，手机和电脑都能直接登录这台设备，不用每次扫码。'));

  async function submit(): Promise<void> {
    if (!canSubmit.value) return;
    error.value = validatePassword(password.value, confirm.value);
    if (error.value) return;
    busy.value = true;
    try {
      // The socket said hello with the pair token, so no current password is needed.
      await access.setPassword(password.value);
      password.value = '';
      confirm.value = '';
      access.finishSetup();
      await router.replace(access.nextLocation());
    } catch (e) {
      error.value = passwordChangeError(e, '设备拒绝了这次修改，请重新扫描设备眼睛屏幕上的二维码后再试。');
    } finally {
      busy.value = false;
    }
  }

  /** Reset mode only: keep the password, use the pair token for this tab. */
  async function skip(): Promise<void> {
    if (!reset.value || busy.value) return;
    access.finishSetup();
    await router.replace(access.nextLocation());
  }

  return { session, password, confirm, show, busy, error, reset, title, sub, canSubmit, submit, skip };
}

export type SetupPasswordPage = ReturnType<typeof useSetupPasswordPage>;
