#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Host tests of the pure half of the voice chain.

    voice_test.py <nyabula_core dir> [--cc CC]

ny_voice_dsp.c (sample formats, pre-roll ring, attach arithmetic, idle gate,
44.1 -> 16 kHz resampler, sentence chunking) and ny_voice_sm.c (the turn
state machine) are compiled exactly as the firmware compiles them, with
nothing else: once plain with -std=c99 -Wall -Wextra -Werror, once under
ASan + UBSan (float casts included).  Nothing here is part of the firmware
build.
"""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('core', type=pathlib.Path)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()

here = pathlib.Path(__file__).resolve().parent
warnings = ['-std=c99', '-Wall', '-Wextra', '-Werror', '-Wshadow', '-Wundef']
sanitizers = ['-g', '-O1', '-fno-omit-frame-pointer',
              '-fsanitize=address,undefined,float-cast-overflow',
              '-fno-sanitize-recover=all']
sources = [str(here / 'voice_test.c'), str(args.core / 'ny_voice_dsp.c'),
           str(args.core / 'ny_voice_sm.c')]
with tempfile.TemporaryDirectory(prefix='nyabot-voice-') as directory:
    root = pathlib.Path(directory)
    for name, flags in (('plain', ['-O2']), ('sanitized', sanitizers)):
        binary = str(root / name)
        subprocess.run([args.cc, *warnings, *flags, '-I' + str(args.core),
                        *sources, '-lm', '-o', binary], check=True)
        subprocess.run([binary], check=True)
print('PASS: voice formats, ring, attach, gate, resampler, sentences, '
      'state machine, sanitizers')
