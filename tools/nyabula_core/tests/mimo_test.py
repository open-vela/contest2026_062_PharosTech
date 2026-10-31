#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile real MiMo code against local HTTP/config fixtures; no cloud calls."""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('core', type=pathlib.Path)
parser.add_argument('agent', type=pathlib.Path)
parser.add_argument('cjson', type=pathlib.Path)
parser.add_argument('mbedtls', type=pathlib.Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='ny-mimo-') as directory:
    root = pathlib.Path(directory)
    (root / 'agent_config.h').write_text(
        '#define AGENT_CFG_KEY_LLM_HOST "llm_host"\n'
        '#define AGENT_CFG_KEY_API_KEY "api_key"\n')
    proxy = (args.agent / 'src/llm/llm_proxy.c').read_text()
    start = proxy.index('const char* llm_mimo_thinking_type(')
    end = proxy.index('int resp_buf_init(', start)
    (root / 'mimo_options.c').write_text(proxy[start:end])
    (root / 'mbed-config.h').write_text('#define MBEDTLS_BASE64_C\n')
    sources = [str(pathlib.Path(__file__).with_suffix('.c')),
               str(args.cjson / 'cJSON.c'), str(args.mbedtls / 'library/base64.c'),
               str(args.mbedtls / 'library/constant_time.c')]
    includes = [root, args.core, args.agent / 'src', args.agent / 'include',
                args.cjson, args.mbedtls / 'include']
    for name, flags in [('plain', ['-O2']), ('sanitized',
        ['-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'])]:
        binary = root / name
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', *flags,
            '-DMBEDTLS_CONFIG_FILE="mbed-config.h"', '-DOK=0', '-DERROR=-1',
            *['-I' + str(path) for path in includes], *sources,
            '-lm', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
