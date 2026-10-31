#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify real password storage, reset and session generation with mbedTLS."""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('core', type=pathlib.Path)
parser.add_argument('mbedtls', type=pathlib.Path)
a = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='ny-auth-') as directory:
    root = pathlib.Path(directory)
    (root / 'nuttx').mkdir()
    (root / 'nuttx/config.h').write_text('#define CONFIG_CRYPTO_MBEDTLS 1\n')
    (root / 'nuttx/mutex.h').write_text(
        '#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n'
        '#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n'
        '#define nxmutex_lock(m) (-pthread_mutex_lock(m))\n'
        '#define nxmutex_unlock(m) (-pthread_mutex_unlock(m))\n')
    (root / 'mbed-config.h').write_text(
        '#define MBEDTLS_MD_C\n#define MBEDTLS_SHA256_C\n'
        '#define MBEDTLS_PKCS5_C\n#define MBEDTLS_CIPHER_C\n')
    fixture = root / 'auth_test.c'
    fixture.write_text(r'''#include "ny_web_auth.c"
#include <assert.h>
int main(int argc, char **argv)
{
  (void)argc;
  char pair[65];
  char before[65];
  char after[65];
  uint32_t retry;
  memset(pair, 'a', 64); pair[64] = 0;
  assert(ny_web_auth_set("first-password") == -EAGAIN);
  assert(ny_web_auth_load(argv[1]) == 0);
  assert(!ny_web_auth_has_password());
  assert(ny_web_auth_generation() == 0);
  assert(ny_web_auth_set("short") == -EINVAL);
  assert(ny_web_auth_generation() == 0);
  assert(ny_web_auth_set("first-password") == 0);
  assert(ny_web_auth_generation() == 1);
  assert(ny_web_auth_session_token(pair, before) == 0);
  assert(ny_web_auth_accept_token(pair, pair, 64));
  assert(ny_web_auth_accept_token(pair, before, 64));
  assert(!ny_web_auth_accept_token(pair, before, 63));
  assert(ny_web_auth_check("first-password", &retry) == 0);
  for (int i=0; i<6; i++)
    assert(ny_web_auth_check("wrong", &retry) == -EACCES);
  assert(ny_web_auth_locked(&retry));
  assert(ny_web_auth_set("second-password") == 0);
  assert(!ny_web_auth_locked(&retry));
  assert(ny_web_auth_generation() == 2);
  assert(ny_web_auth_check("first-password", &retry) == -EACCES);
  assert(ny_web_auth_check("second-password", &retry) == 0);
  assert(ny_web_auth_session_token(pair, after) == 0);
  assert(strcmp(before, after));
  assert(!ny_web_auth_accept_token(pair, before, 64));
  assert(ny_web_auth_accept_token(pair, after, 64));
  assert(ny_web_auth_load(argv[1]) == 0);
  assert(ny_web_auth_check("second-password", &retry) == 0);
  assert(ny_web_auth_load("/no-such-fixture-directory/auth") == 0);
  assert(ny_web_auth_set("third-password") < 0);
  assert(!ny_web_auth_has_password());
  assert(ny_web_auth_generation() == 2);
  puts("NYABULA_AUTH_PASS");
  return 0;
}
''')
    sources = [fixture] + [a.mbedtls / ('library/' + name + '.c')
        for name in ['md', 'sha256', 'pkcs5', 'constant_time', 'platform_util',
                     'cipher', 'cipher_wrap']]
    for label, flags in [('plain', ['-O2']), ('sanitized',
            ['-g', '-O1', '-fsanitize=address,undefined'])]:
        binary = root / label
        subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', *flags,
            '-DMBEDTLS_CONFIG_FILE="mbed-config.h"',
            '-I' + str(root), '-I' + str(a.core),
            '-I' + str(a.mbedtls / 'include'), *map(str, sources),
            '-pthread', '-o', str(binary)], check=True)
        subprocess.run([str(binary), str(root / (label + '.auth'))], check=True)
