#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the production receive hook against a draining, block-based sink."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('core', type=Path)
args = parser.parse_args()
source = (args.core / 'ny_voice.c').read_text()
match = re.search(r'static ssize_t ny_voice_io_recv\([^;]+?\n\{', source)
assert match
end, depth = match.end(), 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
body = source[match.start():end]
defines = '\n'.join(re.findall(
    r'^#define NY_VOICE_(?:OUT_CHUNK|PLAY_RESERVE_MS|PLAY_POLL_MS)\s+\S+',
    source, re.M))
code = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
struct ny_compute_port_s { int unused; };
struct ny_voice_audio_s { size_t queued, partial; };
struct ny_voice_port_s {
  struct ny_compute_port_s *port;
  struct ny_voice_audio_s *playback;
};
static struct ny_voice_audio_s sink;
static int ready, failure, short_write, last_timeout, writes, starved;
static ssize_t ny_compute_port_recv(struct ny_compute_port_s *port,
                                    uint8_t *wire, size_t cap, int timeout)
{
  (void)port; (void)wire; (void)cap;
  last_timeout = timeout;
  if (ready) return ready;
  if (timeout > 0)
    {
      size_t consumed = NY_VOICE_OUT_RATE * NY_VOICE_OUT_CHANNELS * 2 * timeout / 1000;
      if (sink.queued < consumed) { starved++; sink.queued = 0; }
      else sink.queued -= consumed;
    }
  return 0;
}
static size_t ny_voice_audio_queued(struct ny_voice_audio_s *audio)
{
  return audio->queued;
}
static ssize_t ny_voice_audio_write(struct ny_voice_audio_s *audio,
                                     const void *data, size_t bytes, int timeout)
{
  const uint8_t *pcm = data;
  (void)timeout;
  if (failure) return -EPIPE;
  if (short_write) return bytes / 2;
  assert(bytes == NY_VOICE_OUT_CHUNK);
  for (size_t i = 0; i < bytes; i++) assert(pcm[i] == 0);
  audio->partial += bytes;
  while (audio->partial >= NY_VOICE_OUT_CHUNK)
    {
      audio->queued += NY_VOICE_OUT_CHUNK;
      audio->partial -= NY_VOICE_OUT_CHUNK;
    }
  writes++;
  return bytes;
}
'''
test = r'''
int main(void)
{
  uint8_t wire[8];
  struct ny_compute_port_s compute;
  struct ny_voice_port_s port = {.port = &compute, .playback = &sink};
  for (int i = 0; i < 240; i++)
    {
      assert(ny_voice_io_recv(&port, wire, sizeof(wire), 200) == 0);
      assert(last_timeout == NY_VOICE_PLAY_POLL_MS);
      assert(sink.queued < NY_VOICE_OUT_RATE * NY_VOICE_OUT_CHANNELS * 2 / 10 + NY_VOICE_OUT_CHUNK);
    }
  assert(starved == 0 && writes > 0);
  int previous = writes;
  ready = 8;
  assert(ny_voice_io_recv(&port, wire, sizeof(wire), 200) == 8);
  assert(writes == previous && last_timeout == 0);
  ready = -ECONNRESET;
  assert(ny_voice_io_recv(&port, wire, sizeof(wire), 200) == -ECONNRESET);
  ready = 0; sink.queued = 0; failure = 1;
  assert(ny_voice_io_recv(&port, wire, sizeof(wire), 50) == -EPIPE);
  failure = 0; short_write = 1;
  assert(ny_voice_io_recv(&port, wire, sizeof(wire), 50) == -EIO);
  short_write = 0;
  assert(ny_voice_io_recv(&port, wire, sizeof(wire), 0) == 0);
  assert(writes == previous);
  port.playback = NULL;
  assert(ny_voice_io_recv(&port, wire, sizeof(wire), 200) == 0);
  assert(last_timeout == 200 && writes == previous);
  port.port = NULL;
  assert(ny_voice_io_recv(&port, wire, sizeof(wire), 200) == -ENOTCONN);
  puts("VOICE_KEEPALIVE_PASS");
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ny-keepalive-') as directory:
    root = Path(directory)
    for rate, channels in ((44100, 2), (16000, 1)):
        (root / 'test.c').write_text(
            f'#define NY_VOICE_OUT_RATE {rate}U\n'
            f'#define NY_VOICE_OUT_CHANNELS {channels}\n' +
            defines + '\n' + code + body + test)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(root / 'test.c'),
                        '-o', str(root / 'test')], check=True)
        subprocess.run([str(root / 'test')], check=True)
