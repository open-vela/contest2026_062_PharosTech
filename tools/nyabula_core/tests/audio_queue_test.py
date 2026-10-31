#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise production enqueue/dequeue accounting with NuttX's cleared APB."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('core', type=Path)
args = parser.parse_args()


def function(source, name):
    match = re.search(r'static int ' + name + r'\([^;]+?\n\{', source)
    assert match, name
    start = match.start()
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


for module in ('ny_voice_audio', 'ny_audio_stream'):
    source = (args.core / (module + '.c')).read_text()
    upper = module.upper()
    code = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#define NY_PCM_WAV_HEADER_BYTES 44
#define AUDIOIOC_ENQUEUEBUFFER 1
#define AUDIO_MSG_DEQUEUE 2
#define AUDIO_MSG_COMPLETE 3
#define UPPER_FREE 0
#define UPPER_QUEUED 1
#define UPPER_READY 2
struct ap_buffer_s { size_t nbytes, curbyte; unsigned flags; };
struct audio_buf_desc_s {
  size_t numbytes;
  union { struct ap_buffer_s *buffer; } u;
};
struct audio_msg_s { unsigned msg_id; union { void *ptr; } u; };
struct MODULE_buffer_s {
  struct ap_buffer_s *apb;
  size_t submitted;
  unsigned state, order;
};
struct MODULE_s {
  struct { bool capture, wav; } config;
  struct MODULE_buffer_s buffers[2];
  struct { unsigned chunks, starved; } stats;
  unsigned count, arrivals;
  int fd, mq;
  bool complete, started, header_sent;
  size_t queued;
};
struct mq_attr {long mq_curmsgs;};
static struct audio_msg_s message;
static bool pending, reject;
static unsigned empty_receives;
static int fake_getattr(int mq, struct mq_attr *attr){(void)mq;attr->mq_curmsgs=pending?1:0;return 0;}
static int fake_ioctl(int fd, int request, unsigned long pointer)
{
  (void)fd; (void)request; (void)pointer;
  if (reject) { errno = EIO; return -1; }
  return 0;
}
static ssize_t fake_receive(int mq, char *dest, size_t size,
                            unsigned *priority, const struct timespec *until)
{
  (void)mq; (void)size; (void)priority; (void)until;
  if (!pending) { empty_receives++; errno = ETIMEDOUT; return -1; }
  memcpy(dest, &message, sizeof(message)); pending = false;
  return sizeof(message);
}
#define ioctl fake_ioctl
#define mq_timedreceive fake_receive
#define mq_getattr fake_getattr
'''.replace('MODULE', module).replace('UPPER', upper)
    code += function(source, module + '_enqueue') + '\n'
    code += function(source, module + '_pump') + '\n'
    code += r'''
static void returned(struct MODULE_s *stream, unsigned index)
{
  /* audio_dequeuebuffer() resets nbytes before delivering the MQ message. */
  stream->buffers[index].apb->nbytes = 0;
  message.msg_id = AUDIO_MSG_DEQUEUE;
  message.u.ptr = stream->buffers[index].apb;
  pending = true;
  assert(MODULE_pump(stream, 0) == 1);
}
int main(void)
{
  struct ap_buffer_s buffers[2] = {0};
  struct MODULE_s stream = {.count = 2, .started = true};
  for (unsigned i = 0; i < 2; i++) stream.buffers[i].apb = &buffers[i];
  for (unsigned round = 0; round < 128; round++)
    {
      buffers[0].nbytes = 8236;
      buffers[1].nbytes = 8192;
      assert(MODULE_enqueue(&stream, 0) == 0);
      assert(MODULE_enqueue(&stream, 1) == 0);
      assert(stream.queued == 16428);
      returned(&stream, 0);
      assert(stream.queued == 8192);
      /* Duplicate completion must not debit twice. */
      returned(&stream, 0);
      assert(stream.queued == 8192);
      returned(&stream, 1);
      assert(stream.queued == 0);
    }
  reject = true; buffers[0].nbytes = 8192;
  assert(MODULE_enqueue(&stream, 0) == -EIO);
  assert(stream.queued == 0);
  reject = false; stream.config.capture = true;
  buffers[0].nbytes = 320;
  assert(MODULE_enqueue(&stream, 0) == 0);
  message.msg_id = AUDIO_MSG_DEQUEUE; message.u.ptr = &buffers[0];
  pending = true;
  assert(MODULE_pump(&stream, 0) == 1);
  assert(stream.buffers[0].state == UPPER_READY);
  assert(buffers[0].nbytes == 320 && stream.queued == 0);
  puts("MODULE_QUEUE_PASS");
  return 0;
}
'''.replace('MODULE', module).replace('UPPER', upper)
    if module == 'ny_audio_stream':
        code = code.replace('  puts("ny_audio_stream_QUEUE_PASS");', '  assert(empty_receives == 0);\n  puts("NY_AUDIO_NONBLOCKING_EMPTY_QUEUE_PASS");\n  puts("ny_audio_stream_QUEUE_PASS");')
    else:
        code = code.replace('static int fake_getattr', 'static __attribute__((unused)) int fake_getattr')
    with tempfile.TemporaryDirectory(prefix='ny-audio-queue-') as directory:
        root = Path(directory)
        (root / 'test.c').write_text(code)
        for sanitize in (False, True):
            flags = ['-fsanitize=address,undefined'] if sanitize else []
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            *flags, str(root / 'test.c'), '-o', str(root / 'test')],
                           check=True)
            subprocess.run([str(root / 'test')], check=True)
