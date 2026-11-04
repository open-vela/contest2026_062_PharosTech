#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the native renderer's real text fallback selection."""
import re, subprocess, tempfile, sys
from pathlib import Path
source=Path(sys.argv[1]).read_text()
match=re.search(r"static const char \*\s*scene_text\([^;]+?\n\{.*?\n\}",source,re.S)
assert match
code="""#include <stdbool.h>
#include <string.h>
#include <assert.h>
struct nyabula_eye_scene_payload_s { bool live_data; };
"""+match.group()+"""
int main(void) {
 struct nyabula_eye_scene_payload_s p={true};
 assert(!strcmp(scene_text(&p, "", "demo"), ""));
 assert(!strcmp(scene_text(&p, "real", "demo"), "real"));
 p.live_data=false;
 assert(!strcmp(scene_text(&p, "", "demo"), "demo"));
 return 0;
}
"""
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code)
 subprocess.run(['cc','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('SCENE_RENDERER_TEXT_PASS live_empty real_text demo_compatibility')
