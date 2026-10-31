#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real CLI parser and password input with product fixtures."""
import argparse
import json
import os
import pathlib
import pty
import select
import subprocess
import tempfile
import termios
import time

parser = argparse.ArgumentParser()
parser.add_argument('core', type=pathlib.Path)
parser.add_argument('cjson', type=pathlib.Path)
a = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='ny-cli-') as directory:
    root = pathlib.Path(directory)
    (root / 'nuttx').mkdir()
    (root / 'netutils').mkdir()
    (root / 'nuttx/config.h').write_text(
        '#define CONFIG_NYABULA_CORE_WEB 1\n'
        '#define CONFIG_NYABULA_CORE_WEB_AUTH_PATH "/tmp/fixture-auth"\n')
    (root / 'netutils/cJSON.h').symlink_to(a.cjson / 'cJSON.h')
    fixture = root / 'fixture.c'
    fixture.write_text(r'''#include "ny_product.h"
#include "ny_web_auth.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
int ny_product_request(const struct ny_product_caller_s *caller,
 const char *topic, const cJSON *data, cJSON **result)
{
  if (caller->role != NY_PRODUCT_OWNER || !caller->local_transport)
    return -EACCES;
  *result = cJSON_Duplicate(data, true);
  cJSON_AddStringToObject(*result, "topic", topic);
  return !strcmp(topic, "fixture.error") ? -EIO : 0;
}
int ny_product_cli(const char *topic, const char *path)
{
  printf("FILE %s %s\n", topic, path);
  return 0;
}
bool ny_web_auth_has_password(void) { return true; }
bool ny_web_auth_locked(uint32_t *retry) { *retry=0; return false; }
int ny_web_panel_count(void) { return 0; }
int ny_web_auth_load(const char *path) { (void)path; return 0; }
int ny_web_auth_set(const char *password)
{
  if (strlen(password) < 8 || strlen(password) > 64) return -EINVAL;
  puts("AUTH_SET"); return 0;
}
''')
    binary = root / 'nyabula'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I' + str(root), '-I' + str(a.core), str(a.core / 'nyabula_main.c'),
        str(fixture), str(a.cjson / 'cJSON.c'), '-lm', '-o', str(binary)], check=True)
    def run(args, text='', ok=True):
        result = subprocess.run([str(binary), *args], input=text,
                                capture_output=True, text=True)
        assert (result.returncode == 0) == ok, result
        return result.stdout
    def request(args, topic, **fields):
        output = json.loads(run(args))
        assert output == dict(topic=topic, **fields), output
    request(['status'], 'sys.info')
    request(['thinking', 'set', 'none'], 'agent.config.thinking.set', level='none')
    request(['voice', 'on'], 'voice.enable', enabled=True)
    request(['voice', 'off'], 'voice.enable', enabled=False)
    request(['voice', 'say', 'hello "cat"'], 'voice.say', text='hello "cat"')
    request(['panel', 'qr'], 'panel.pair.show')
    request(['panel', 'hide'], 'panel.pair.hide')
    request(['network', 'name', 'Nyabula'], 'network.name.set', name='Nyabula')
    assert 'FILE agent.config.mimo.set /tmp/settings.json' in run(
        ['mimo', 'configure', '/tmp/settings.json'])
    assert 'password_set=yes' in run(['panel', 'status'])
    output = run(['panel', 'reset-password'], 'fixture-password\nfixture-password\n')
    assert 'AUTH_SET' in output and 'fixture-password' not in output
    for newline in ['\r', '\r\n']:
        output = run(['panel', 'reset-password'],
                     newline.join(['fixture-password', 'fixture-password', '']))
        assert 'AUTH_SET' in output and 'fixture-password' not in output
    assert 'AUTH_SET' in run(['panel', 'reset-password'],
                            'fixture-passworX\bd\nfixture-password\n')
    for text in ['one\ntwo\n', 'short\nshort\n', '', 'x' * 80 + '\n']:
        assert 'AUTH_SET' not in run(['panel', 'reset-password'], text, False)
    assert 'Nyabula' in run([], '0\n')
    assert 'AUTH_SET' in run([], '2\nfixture-password\nfixture-password\n0\n')
    for args in [['unknown'], ['voice', 'on', 'extra'], ['thinking', 'set'],
                 ['panel', 'reset-password', 'unsafe-argument']]:
        run(args, ok=False)
    assert 'fixture.error' in run(['request', 'fixture.error'], ok=False)
    for cancel in [False, True]:
        master, slave = pty.openpty()
        original = termios.tcgetattr(slave)
        child = subprocess.Popen([str(binary), 'panel', 'reset-password'],
                                 stdin=slave, stdout=slave, stderr=slave)
        transcript = bytearray()
        def until(marker):
            end = time.monotonic() + 3
            while marker not in transcript and time.monotonic() < end:
                if select.select([master], [], [], .1)[0]:
                    transcript.extend(os.read(master, 4096))
            assert marker in transcript, transcript
        until(b'New panel password')
        assert not (termios.tcgetattr(slave)[3] & termios.ECHO)
        if cancel:
            os.write(master, b'\x03')
        else:
            os.write(master, b'fixture-password\r')
            until(b'Repeat password')
            os.write(master, b'fixture-password\r')
            until(b'AUTH_SET')
        assert child.wait(timeout=3) == (1 if cancel else 0)
        assert termios.tcgetattr(slave) == original
        assert b'fixture-password' not in transcript
        os.close(master)
        os.close(slave)
    print('NYABULA_CLI_PASS')
