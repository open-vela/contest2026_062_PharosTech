#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Same UTF-8 edge vectors before and after extraction of the WebSocket helper."""
from pathlib import Path
import subprocess
import sys
import tempfile

core = Path(sys.argv[1])
source = (core/'ny_websocket.c').read_text()
function = source[source.index('bool nyabula_eye_ws_utf8('):]
with tempfile.TemporaryDirectory(prefix='nyabula-utf8-') as directory:
    root = Path(directory)
    header = '#include "ny_utf8.h"\n' if (core/'ny_utf8.h').exists() else ''
    (root/'test.c').write_text('#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n#include <assert.h>\n#include <stdio.h>\n' +
       header + function + r'''
int main(void) {
  const unsigned char *good[] = {(const unsigned char *)"", (const unsigned char *)"text",
    (const unsigned char *)"\xc2\x80", (const unsigned char *)"\xe4\xb8\xad",
    (const unsigned char *)"\xf0\x90\x80\x80", (const unsigned char *)"\xf4\x8f\xbf\xbf"};
  const size_t sizes[] = {0,4,2,3,4,4};
  for (size_t i=0;i<6;i++) assert(nyabula_eye_ws_utf8(good[i],sizes[i]));
  const unsigned char bad[][4] = {{0x80},{0xc0,0x80},{0xe0,0x80,0x80},
    {0xed,0xa0,0x80},{0xf4,0x90,0x80,0x80},{0xff},{0xf0,0x90},{0xc2,0x7f}};
  const size_t lengths[] = {1,2,3,3,4,1,2,2};
  for (size_t i=0;i<8;i++) assert(!nyabula_eye_ws_utf8(bad[i],lengths[i]));
  puts("PASS: shared UTF-8 valid boundaries, overlong, surrogate, out-of-range, truncated, continuation");
}''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-no-pie','-fsanitize=address,undefined',
                    '-I'+str(core),str(root/'test.c'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
