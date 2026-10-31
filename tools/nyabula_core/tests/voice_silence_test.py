#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the production cloud-ASR submission gate without a microphone."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('core', type=Path)
a = p.parse_args()
source = (a.core/'ny_voice.c').read_text()
match = re.search(r'static void ny_voice_turn_cloud_end\([^;]+?\n\{', source)
assert match
end, depth = match.end(), 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
function = source[match.start():end]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define NY_VOICE_EV_ASR_FINISHED 1
struct ny_voice_turn_s { int16_t *cloud_pcm; bool asr_open, cloud_heard; size_t cloud_samples; unsigned cloud_request; };
static int submitted, posted, failed, fail_submit;
static int16_t *owned;
static int ny_voice_cloud_submit(int16_t *pcm,size_t length,unsigned *request)
{ assert(length==16000); submitted++; if(fail_submit)return -5; owned=pcm; *request=42; return 0; }
static void ny_voice_post(int event,bool success,bool final,int unused)
{ assert(event==1 && final && !unused); posted++; failed+=!success; }
static void ny_voice_error(int error,const char *message) { assert(error==-5 && message); }
'''
test = r'''
int main(void)
{
  struct ny_voice_turn_s turn={.cloud_pcm=calloc(16000,2),.asr_open=true,.cloud_samples=16000};
  ny_voice_turn_cloud_end(&turn);
  assert(!turn.cloud_pcm && !turn.asr_open && submitted==0 && posted==1 && failed==0);
  turn.cloud_pcm=calloc(16000,2); turn.asr_open=true; turn.cloud_heard=true;
  ny_voice_turn_cloud_end(&turn);
  assert(!turn.cloud_pcm && turn.asr_open && submitted==1 && turn.cloud_request==42);
  ny_voice_turn_cloud_end(&turn); assert(submitted==1); free(owned);
  turn.cloud_pcm=calloc(16000,2); fail_submit=1;
  ny_voice_turn_cloud_end(&turn);
  assert(!turn.cloud_pcm && !turn.asr_open && submitted==2 && posted==2 && failed==1);
  puts("VOICE_CLOUD_SILENCE_SKIP_OWNERSHIP_ERROR_PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='ny-voice-silence-') as directory:
    root=Path(directory)
    (root/'test.c').write_text(code+function+test)
    for flags in ([], ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']):
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',*flags,
                        str(root/'test.c'),'-o',str(root/'test')],check=True)
        subprocess.run([str(root/'test')],check=True)
