#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real cloud ASR worker's ownership and cancellation contract."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('core', type=Path)
args = parser.parse_args()
code = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int (*worker)(int, char **);
static int spawn_error, request_error, calls;
int task_create(const char *name, int priority, int stack,
                int (*entry)(int, char **), char **argv)
{
  (void)name; (void)priority; (void)stack; (void)argv;
  if (spawn_error) { errno = EAGAIN; return -1; }
  worker = entry; return 123;
}
int ny_mimo_transcribe(const int16_t *pcm, size_t count, unsigned int rate,
                       char *text, size_t capacity)
{
  assert(count == 4 && pcm[0] == 17 && rate == 16000);
  calls++;
  snprintf(text, capacity, "Recognized speech");
  return request_error;
}
#include "ny_voice_cloud.c"
static int16_t *samples(void)
{
  int16_t *pcm = calloc(4, sizeof(int16_t)); assert(pcm); pcm[0] = 17;
  return pcm;
}
int main(void)
{
  uint32_t first, second;
  char text[64];
  int16_t *pcm = samples();
  spawn_error = 1;
  assert(ny_voice_cloud_submit(pcm, 4, &first) == -EAGAIN);
  free(pcm);
  spawn_error = 0;
  assert(ny_voice_cloud_submit(samples(), 4, &first) == 0);
  assert(ny_voice_cloud_poll(first, text, sizeof(text)) == 0);
  pcm = samples();
  assert(ny_voice_cloud_submit(pcm, 4, &second) == -EBUSY);
  free(pcm);
  ny_voice_cloud_cancel(first);
  assert(ny_voice_cloud_poll(first, text, sizeof(text)) == -ECANCELED);
  worker(0, NULL);
  assert(ny_voice_cloud_poll(first, text, sizeof(text)) == -ECANCELED);
  assert(ny_voice_cloud_submit(samples(), 4, &second) == 0);
  assert(second != first);
  assert(ny_voice_cloud_poll(first, text, sizeof(text)) == -ESTALE);
  ny_voice_cloud_cancel(first);
  worker(0, NULL);
  assert(ny_voice_cloud_poll(second, text, 3) == -EOVERFLOW);
  assert(ny_voice_cloud_poll(second, text, sizeof(text)) == 1);
  assert(!strcmp(text, "Recognized speech"));
  assert(ny_voice_cloud_submit(samples(), 4, &second) == 0);
  request_error = -EACCES; worker(0, NULL);
  assert(ny_voice_cloud_poll(second, text, sizeof(text)) == -EACCES);
  assert(calls == 3 && !g_cloud_busy && !g_cloud_pcm);
  puts("VOICE_CLOUD_OWNERSHIP_CANCEL_STALE_PASS");
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ny-voice-cloud-') as directory:
    root = Path(directory)
    (root / 'nuttx').mkdir()
    (root / 'nuttx/mutex.h').write_text(
        '#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n'
        '#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n'
        '#define nxmutex_lock pthread_mutex_lock\n'
        '#define nxmutex_unlock pthread_mutex_unlock\n')
    (root / 'cJSON.h').write_text('typedef struct cJSON cJSON;\n')
    (root / 'test.c').write_text(code)
    for name, flags in [('plain', ['-O2']), ('sanitized',
            ['-g', '-O1', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all'])]:
        binary = root / name
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        *flags, '-I' + str(root), '-I' + str(args.core),
                        str(root / 'test.c'), '-pthread', '-o', str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)
