#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Host test of the Bluetooth audio pieces that need no board.

usage: bt_audio_test.py <app/nyabula_core> <external/libfluoride-sbc/libfluoride-sbc>

Builds ny_sbc.c and ny_pcm.c exactly as the firmware does, against the SBC
library of the openvela tree, with the address and undefined behaviour
sanitizers on, and runs bt_audio_test.c.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

GOLDEN_SBC = '0x48b37149'
GOLDEN_MSBC = '0xb09ac0c5'

if len(sys.argv) != 3:
    sys.exit(__doc__)
core = Path(sys.argv[1]).resolve()
sbc = Path(sys.argv[2]).resolve()
tests = Path(__file__).resolve().parent
decoder = sorted((sbc/'decoder'/'srce').glob('*.c'))
encoder = sorted((sbc/'encoder'/'srce').glob('*.c'))
if not decoder or not encoder:
    sys.exit('no SBC library sources under %s' % sbc)
with tempfile.TemporaryDirectory(prefix='nyabula-bt-audio-') as directory:
    root = Path(directory)
    library = []
    for source in decoder + encoder:
        target = root/(source.parent.parent.name + '-' + source.stem + '.o')
        # The library is third party code: built as the firmware builds it,
        # without our warning set and without the sanitizers.
        subprocess.run(['cc', '-std=gnu11', '-O2', '-w', '-c', str(source),
                        '-I' + str(sbc/'decoder'/'include'),
                        '-I' + str(sbc/'encoder'/'include'),
                        '-o', str(target)], check=True)
        library.append(str(target))
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-no-pie', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=undefined',
                    '-DNY_GOLDEN_SBC=' + GOLDEN_SBC,
                    '-DNY_GOLDEN_MSBC=' + GOLDEN_MSBC,
                    '-I' + str(core), '-I' + str(tests),
                    '-I' + str(sbc/'decoder'/'include'),
                    '-I' + str(sbc/'encoder'/'include'),
                    str(tests/'bt_audio_test.c'), str(core/'ny_sbc.c'),
                    str(core/'ny_pcm.c')] + library +
                   ['-lm', '-o', str(root/'test')], check=True)
    subprocess.run([str(root/'test')], check=True)
