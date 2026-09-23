import test from 'node:test';
import assert from 'node:assert/strict';
import {
  decideBoot, formatCountdown, guardRedirect, lockDeadline, lockSecondsLeft, parseAuthState, retryAfterOf, safeResumePath, validatePassword,
} from '../src/lib/deviceAccess.ts';

const boot = (hasPairToken, hasSessionToken, helloResult, passwordSet = false) => decideBoot({ hasPairToken, hasSessionToken, passwordSet, helloResult });

test('boot: no credential at all goes to the login page', () => {
  for (const helloResult of ['none', 'ok', 'rejected', 'failed']) {
    assert.deepEqual(boot(false, false, helloResult), { action: 'login', forget: null });
    assert.deepEqual(boot(false, false, helloResult, true), { action: 'login', forget: null });
  }
});

test('boot: a pair token wins over a stored session and leads to the password page', () => {
  assert.deepEqual(boot(true, false, 'none'), { action: 'hello', credential: 'pair' });
  assert.deepEqual(boot(true, true, 'none'), { action: 'hello', credential: 'pair' });
  assert.deepEqual(boot(true, false, 'ok', false), { action: 'setup', mode: 'set' });
  assert.deepEqual(boot(true, true, 'ok', true), { action: 'setup', mode: 'reset' });
});

test('boot: a refused pair token falls back to the stored session, else to login', () => {
  assert.deepEqual(boot(true, true, 'rejected'), { action: 'restart', forget: 'pair' });
  assert.deepEqual(boot(true, false, 'rejected'), { action: 'login', forget: 'pair' });
});

test('boot: a stored session enters, and is forgotten when the device refuses it', () => {
  assert.deepEqual(boot(false, true, 'none', true), { action: 'hello', credential: 'session' });
  assert.deepEqual(boot(false, true, 'ok', true), { action: 'enter' });
  assert.deepEqual(boot(false, true, 'rejected', true), { action: 'login', forget: 'session' });
});

test('boot: link trouble during hello is unreachable, never a forgotten credential', () => {
  assert.deepEqual(boot(true, true, 'failed'), { action: 'unreachable' });
  assert.deepEqual(boot(false, true, 'failed'), { action: 'unreachable' });
});

test('guard: before access is granted exactly one page is reachable', () => {
  const pages = ['connect', 'login', 'setup-password', 'provision', 'home', 'services', 'settings', 'account', null];
  for (const [phase, only] of [['boot', 'connect'], ['login', 'login'], ['setup', 'setup-password']]) {
    for (const online of [true, false, null]) {
      for (const name of pages) assert.equal(guardRedirect(name, phase, online), name === only ? null : only);
    }
  }
});

test('guard: an offline device seals every route onto /provision', () => {
  for (const name of ['home', 'eye', 'services', 'plugins', 'agent', 'device', 'workspace-settings', 'settings', 'account', 'connect', 'login', 'setup-password', null]) {
    assert.equal(guardRedirect(name, 'ready', false), 'provision');
  }
  assert.equal(guardRedirect('provision', 'ready', false), null);
});

test('guard: online (or unknown) lets everything through, gate pages go home', () => {
  for (const online of [true, null]) {
    for (const name of ['home', 'services', 'device', 'settings', 'account', 'provision']) assert.equal(guardRedirect(name, 'ready', online), null);
    for (const name of ['connect', 'login', 'setup-password']) assert.equal(guardRedirect(name, 'ready', online), 'home');
  }
});

test('resume path: in-app paths only, never a gate page', () => {
  assert.equal(safeResumePath('/d/self/features/alarm'), '/d/self/features/alarm');
  assert.equal(safeResumePath('/provision'), '/provision');
  assert.equal(safeResumePath('/d/self?tab=1'), '/d/self?tab=1');
  for (const bad of ['/', '/connect', '/login', '/login?x=1', '/setup-password', '//evil.example/x', 'http://evil.example', 'd/self', '', null, undefined, 7]) {
    assert.equal(safeResumePath(bad), null);
  }
});

test('password: 8..64, confirmation must match', () => {
  assert.equal(validatePassword('abcdefgh'), null);
  assert.equal(validatePassword('a'.repeat(64)), null);
  assert.equal(validatePassword('abcdefgh', 'abcdefgh'), null);
  assert.match(validatePassword(''), /请输入/);
  assert.match(validatePassword('abcdefg'), /至少 8/);
  assert.match(validatePassword('a'.repeat(65)), /最多 64/);
  assert.match(validatePassword('abcdefgh', 'abcdefgH'), /不一致/);
  // Too short wins over the mismatch: fix the password first.
  assert.match(validatePassword('abc', 'abd'), /至少 8/);
});

test('password: characters for the minimum, UTF-8 bytes for the maximum', () => {
  assert.match(validatePassword('密码密码'), /至少 8/); // 12 bytes but only 4 characters
  assert.equal(validatePassword('密码密码密码密码'), null); // 8 characters, 24 bytes
  assert.equal(validatePassword('密'.repeat(21)), null); // 63 bytes
  assert.match(validatePassword('密'.repeat(22)), /最多 64/); // 66 bytes
});

test('auth state parsing tolerates junk', () => {
  assert.deepEqual(parseAuthState({ passwordSet: true, locked: false, retryAfterMs: 0 }), { passwordSet: true, locked: false, retryAfterMs: 0 });
  assert.deepEqual(parseAuthState({ passwordSet: true, locked: true, retryAfterMs: 1500.2 }), { passwordSet: true, locked: true, retryAfterMs: 1501 });
  // A positive wait means locked even if the flag is missing.
  assert.deepEqual(parseAuthState({ retryAfterMs: 10 }), { passwordSet: false, locked: true, retryAfterMs: 10 });
  for (const junk of [null, undefined, 'x', 7, [], { passwordSet: 'yes', locked: 1, retryAfterMs: '5' }]) {
    assert.deepEqual(parseAuthState(junk), { passwordSet: false, locked: false, retryAfterMs: 0 });
  }
});

test('lock countdown maths', () => {
  assert.equal(retryAfterOf({ code: 'ELOCKED', retryAfterMs: 30000 }), 30000);
  for (const junk of [null, {}, { retryAfterMs: -5 }, { retryAfterMs: NaN }, { retryAfterMs: Infinity }, { retryAfterMs: '9' }]) assert.equal(retryAfterOf(junk), 0);

  assert.equal(lockDeadline(1000, 0), 0);
  assert.equal(lockDeadline(1000, 30000), 31000);

  const deadline = lockDeadline(1_000_000, 30_000);
  assert.equal(lockSecondsLeft(deadline, 1_000_000), 30);
  assert.equal(lockSecondsLeft(deadline, 1_000_001), 30); // rounds up: never unlocks early
  assert.equal(lockSecondsLeft(deadline, 1_029_000), 1);
  assert.equal(lockSecondsLeft(deadline, 1_029_999), 1);
  assert.equal(lockSecondsLeft(deadline, 1_030_000), 0);
  assert.equal(lockSecondsLeft(deadline, 1_999_999), 0);
  assert.equal(lockSecondsLeft(0, 5), 0);

  assert.equal(formatCountdown(0), '0 秒');
  assert.equal(formatCountdown(59), '59 秒');
  assert.equal(formatCountdown(60), '1 分 00 秒');
  assert.equal(formatCountdown(125), '2 分 05 秒');
  assert.equal(formatCountdown(-3), '0 秒');
});
