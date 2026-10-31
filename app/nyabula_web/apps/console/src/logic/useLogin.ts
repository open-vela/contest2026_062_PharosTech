/* Login / register form state (cloud.md §4 auth endpoints). Validation is
 * client-side sugar only; the server decides. */
import { ref } from 'vue';
import { useAuthStore } from '../stores/auth';

export function useLogin() {
  const auth = useAuthStore();
  const mode = ref<'login' | 'register'>('login');
  const email = ref('');
  const password = ref('');
  const name = ref('');
  const localError = ref<string | null>(null);

  function validate(): string | null {
    if (!email.value.trim()) return '请输入邮箱';
    if (!password.value) return '请输入密码';
    if (mode.value === 'register' && !name.value.trim()) return '请输入昵称';
    return null;
  }

  /** Resolves true when signed in. */
  async function submit(): Promise<boolean> {
    if (auth.busy) return false;
    localError.value = validate();
    if (localError.value) return false;
    return mode.value === 'login'
      ? auth.login(email.value.trim(), password.value)
      : auth.register(email.value.trim(), password.value, name.value.trim());
  }

  function setMode(m: 'login' | 'register'): void {
    mode.value = m;
    localError.value = null;
    auth.lastError = null;
  }

  return { auth, mode, setMode, email, password, name, localError, submit };
}
