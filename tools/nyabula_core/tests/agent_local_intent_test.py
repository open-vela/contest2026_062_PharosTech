#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Host tests of the on-device intent rules (ny_agent_local_intent.c).

    agent_local_intent_test.py <nyabula_core dir> <dir holding cJSON.c/.h>
                               [--fuzz N] [--seed S] [--cc CC]

The real source is compiled with cJSON and nothing else: once plain with
-std=c99 -Wall -Wextra -Werror, once under ASan + UBSan (float casts
included).  Both run the tables; the sanitizer build also runs the fuzz loop.
Nothing here is part of the firmware build.
"""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('core', type=pathlib.Path)
parser.add_argument('cjson', type=pathlib.Path)
parser.add_argument('--fuzz', type=int, default=200000)
parser.add_argument('--seed', type=int, default=1)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()

here = pathlib.Path(__file__).resolve().parent
warnings = ['-std=c99', '-Wall', '-Wextra', '-Werror']
sanitizers = ['-g', '-O1', '-fno-omit-frame-pointer',
              '-fsanitize=address,undefined,float-cast-overflow',
              '-fno-sanitize-recover=all']
with tempfile.TemporaryDirectory(prefix='nyabot-intent-') as directory:
    root = pathlib.Path(directory)
    # The firmware spells it <netutils/cJSON.h>; cJSON.c wants "cJSON.h".
    (root / 'netutils').mkdir()
    (root / 'netutils/cJSON.h').symlink_to((args.cjson / 'cJSON.h').resolve())
    sources = [str(here / 'agent_local_intent_test.c'),
               str(args.core / 'ny_agent_local_intent.c'),
               str(args.cjson / 'cJSON.c')]
    include = ['-I' + str(root), '-I' + str(root / 'netutils'),
               '-I' + str(args.core)]
    for name, flags in (('plain', ['-O2']), ('sanitized', sanitizers)):
        binary = str(root / name)
        subprocess.run([args.cc, *warnings, *flags, *include, *sources,
                        '-lm', '-o', binary], check=True)
        subprocess.run([binary], check=True)
    if args.fuzz > 0:
        subprocess.run([str(root / 'sanitized'), 'fuzz', str(args.fuzz),
                        str(args.seed)], check=True)
print('PASS: intent rules, call/reply/fact shapes, sanitizers, fuzz')
