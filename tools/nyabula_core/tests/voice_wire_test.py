#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The control domain's speech client against nyampd's real speech services.

    voice_wire_test.py <repo> [--cc CC] [--cxx CXX]

tools/amp/test_voice_flow.py proves the wire with a client written in Python
from the protocol document.  This runs the same service side -- its harness,
taken from that script so the two cannot drift apart: nyampd's Dispatch with
the real AsrService, TtsService and KwsService over a SOCK_SEQPACKET pair and
a file mapped as the 4 MiB arena -- against the client the firmware ships,
app/nyabula_core/ny_voice_wire.c, built with -Wall -Wextra -Werror under ASan
and UBSan.  Only the models are scripted.  Nothing here is part of the
firmware build.
"""
import argparse
import pathlib
import re
import socket
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('repo', type=pathlib.Path)
parser.add_argument('--cc', default='gcc')
parser.add_argument('--cxx', default='g++')
args = parser.parse_args()

repo = args.repo.resolve()
here = pathlib.Path(__file__).resolve().parent
amp = repo / 'tools/amp'
nyampd = amp / 'nyampd'
core = repo / 'app/nyabula_core'
flow = (amp / 'test_voice_flow.py').read_text(encoding='utf-8')
layout = (repo / 'chips/rk3576/include/rk3576_shmem_layout.h').read_text(
    encoding='utf-8')

defines = dict(re.findall(r'#define\s+(NYAMP_\w+)\s+(0x[0-9a-fA-F]+)U', layout))
harness = re.search(r'write_text\(r\'\'\'(.*?)\'\'\', encoding', flow, re.S)
sources = re.search(r'^sources = (\[.*?\])$', flow, re.S | re.M)
chat = re.search(r'^chat_sources = (\[.*?\])$', flow, re.S | re.M)
generation = re.search(r'^GENERATION = (\w+)$', flow, re.M)
assert harness and sources and chat and generation, 'test_voice_flow.py moved'

warnings = ['-Wall', '-Wextra', '-Werror']
sanitizers = ['-g', '-O1', '-fno-omit-frame-pointer',
              '-fsanitize=address,undefined,float-cast-overflow',
              '-fno-sanitize-recover=all']
with tempfile.TemporaryDirectory(prefix='nyabot-voice-wire-') as directory:
    root = pathlib.Path(directory)
    (root / 'harness.cpp').write_text(harness.group(1), encoding='utf-8')
    subprocess.run([args.cc, '-std=c11', '-O1', *warnings, '-c',
                    str(amp / 'protocol/nyamp_protocol.c'),
                    '-o', str(root / 'protocol.o')], check=True)
    subprocess.run(
        [args.cxx, '-std=c++17', '-O1', *warnings,
         '-I', str(nyampd), '-I', str(amp / 'models'),
         '-I', str(amp / 'protocol'), '-I', str(amp / 'chat'),
         '-I', str(amp / 'g2p'), str(root / 'harness.cpp'),
         *[str(nyampd / name) for name in eval(sources.group(1))],
         *[str(amp / 'chat' / name) for name in eval(chat.group(1))],
         str(amp / 'models/nyamp_models.cpp'), str(amp / 'g2p/nyamp_g2p.cpp'),
         str(root / 'protocol.o'), '-o', str(root / 'harness'), '-lpthread'],
        check=True)
    subprocess.run(
        [args.cc, '-std=c99', '-D_POSIX_C_SOURCE=200809L', *warnings,
         '-Wshadow', '-Wundef', *sanitizers, '-I', str(core),
         '-I', str(amp / 'protocol'), str(here / 'voice_wire_test.c'),
         str(core / 'ny_voice_wire.c'), str(core / 'ny_voice_dsp.c'),
         str(amp / 'protocol/nyamp_protocol.c'), '-lm',
         '-o', str(root / 'client')], check=True)

    arena = root / 'arena'
    with open(arena, 'wb') as handle:
        handle.truncate(4 * 1024 * 1024)
    ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    service = subprocess.Popen(
        [str(root / 'harness'), str(theirs.fileno()), str(arena),
         str(int(generation.group(1), 0)), defines['NYAMP_SLOT_SHARED'],
         defines['NYAMP_SLOT_SHARED_SIZE'], defines['NYAMP_SLOT_CAPTURE'],
         defines['NYAMP_SLOT_CAPTURE_SIZE']], pass_fds=[theirs.fileno()])
    try:
        subprocess.run(
            [str(root / 'client'), str(ours.fileno()), str(arena),
             str(int(generation.group(1), 0)), defines['NYAMP_SLOT_SHARED'],
             defines['NYAMP_SLOT_CAPTURE']], pass_fds=[ours.fileno()],
            check=True, timeout=120)
    finally:
        ours.close()
        theirs.close()
        try:
            service.wait(timeout=20)
        except subprocess.TimeoutExpired:
            service.terminate()
print('PASS: ny_voice_wire.c against the real ASR, KWS and TTS services')
