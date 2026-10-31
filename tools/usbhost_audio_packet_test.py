#!/usr/bin/env python3
# tools/usbhost_audio_packet_test.py
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements. See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to you under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Exercise the actual USB playback packetizer across application buffers."""
import pathlib
import re
import subprocess
import sys
import tempfile

source = pathlib.Path(sys.argv[1]).read_text()
match = re.search(r"static int usbhost_audio_transfer\([^;]+?\)\s*\{", source)
assert match
end, depth = match.end(), 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
function = source[match.start():end]

stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#define FAR
#define OK 0
#define USBHOST_AUDIO_PLAYBACK 0
#define USBHOST_AUDIO_CAPTURE 1
#define USB_SPEED_HIGH 3
#define USB_SPEED_SUPER 4
#define USB_EP_MAX_PACKET_MASK 0x7ff
#define USB_EP_MAX_PACKET_MULT(x) (((x) >> 11) & 3)
#define AUDIO_APB_FINAL 1
#define DRVR_IOALLOC(driver, buffer, size) (-ENOMEM)
struct usbhost_hubport_s { void *drvr; int speed; };
struct epdesc { unsigned mxpacketsize, maxburst, interval; };
struct usbhost_audio_format_s { struct epdesc epdesc; unsigned channels, subslot; };
struct usbhost_audio_s { struct {struct usbhost_hubport_s *hport;} usbclass; bool disconnected; };
struct usbhost_audio_stream_s {
 struct usbhost_audio_format_s format[1];
 struct usbhost_audio_s *audio;
 unsigned current, direction, rate, accumulator, feedbackrate;
 bool terminate;
 void *ep;
 uint8_t *packetbuf;
 size_t packetsize, captureused, captureoffset, playbackused, playbacksize;
};
struct ap_buffer_s { unsigned curbyte, nbytes, nmaxbytes, flags; uint8_t samp[4096]; };
static unsigned packets, received, total, rate, phase, padding;
static int failure;
static bool pending;
static bool capture;
static unsigned capturevalue, capturecalls;
static int capturefailafter = -1;
bool usbhost_audio_pending(struct usbhost_audio_stream_s *s) { (void)s; return pending; }
static ssize_t transfer(void *driver, void *endpoint, uint8_t *data, size_t bytes)
{
 (void)driver; (void)endpoint;
 if (capture) {
   const unsigned sizes[] = {188,196,192,0};
   if (capturefailafter >= 0 && capturecalls >= (unsigned)capturefailafter) return -EIO;
   unsigned count = sizes[capturecalls++ % 4];
   assert(count <= bytes);
   for (unsigned i=0;i<count;i++) data[i]=(uint8_t)(capturevalue++ % 251);
   return count;
 }
 if (failure) return failure == 1 ? (ssize_t)bytes - 4 : -EIO;
 phase += rate;
 unsigned expected = (phase / 1000) * 4;
 phase %= 1000;
 if (bytes != expected) {
   fprintf(stderr, "packet %u: expected %u bytes, got %zu at application boundary\n",
           packets, expected, bytes);
   exit(1);
 }
 for (size_t i = 0; i < bytes; i++) {
   if (received < total) assert(data[i] == (uint8_t)(received++ % 251));
   else { assert(data[i] == 0); padding++; }
 }
 packets++;
 return bytes;
}
#define DRVR_TRANSFER transfer
'''
tests = r'''
static void run(unsigned hz, unsigned chunk, unsigned bytes, bool final)
{
 struct usbhost_hubport_s h = {.speed=2};
 struct usbhost_audio_s a = {.usbclass={.hport=&h}};
 uint8_t packet[200];
 struct usbhost_audio_stream_s s = {
  .audio=&a, .direction=USBHOST_AUDIO_PLAYBACK, .rate=hz,
  .format={{{200,0,1},2,2}}, .packetbuf=packet, .packetsize=sizeof(packet)
 };
 struct ap_buffer_s b = {0};
 packets=received=padding=phase=0; rate=hz; total=bytes;
 for (unsigned offset=0; offset<bytes;) {
   unsigned count = bytes-offset < chunk ? bytes-offset : chunk;
   b.curbyte=0; b.nbytes=count; b.flags=final && offset+count==bytes ? AUDIO_APB_FINAL : 0;
   pending=offset+count<bytes;
   for (unsigned i=0;i<count;i++) b.samp[i]=(uint8_t)((offset+i)%251);
   assert(usbhost_audio_transfer(&s,&b)==0);
   assert(b.curbyte==count);
   offset+=count;
 }
 assert(received==bytes && s.playbackused==0 && s.playbacksize==0);
 if (bytes==hz*4) assert(packets==1000 && padding==0);
 else assert(padding<200);
 printf("PASS rate=%u chunk=%u source_bytes=%u packets=%u padding=%u final=%d\n",
        hz,chunk,bytes,packets,padding,final);
 b.curbyte=0; b.nbytes=3;
 assert(usbhost_audio_transfer(&s,&b)==-EINVAL);
 b.curbyte=0; b.nbytes=192; b.flags=AUDIO_APB_FINAL;
 failure=1;
 assert(usbhost_audio_transfer(&s,&b)==-EIO);
 assert(s.playbackused==0 && s.playbacksize==0);
 failure=2; b.curbyte=0;
 assert(usbhost_audio_transfer(&s,&b)==-EIO);
 assert(s.playbackused==0 && s.playbacksize==0);
 failure=0;
 s.packetbuf=NULL; b.curbyte=0;
 assert(usbhost_audio_transfer(&s,&b)==-ENOMEM);
}
static void capture_regression(void)
{
 struct usbhost_hubport_s h={.speed=2};
 struct usbhost_audio_s a={.usbclass={.hport=&h}};
 uint8_t packet[196];
 struct usbhost_audio_stream_s s={
  .audio=&a,.direction=USBHOST_AUDIO_CAPTURE,.rate=48000,
  .format={{{196,0,1},2,2}},.packetbuf=packet,.packetsize=196
 };
 struct ap_buffer_s b={.nmaxbytes=1000};
 unsigned expected=0;
 capture=true;
 for (unsigned round=0;round<20;round++) {
   assert(usbhost_audio_transfer(&s,&b)==0 && b.nbytes==1000);
   for(unsigned i=0;i<b.nbytes;i++) assert(b.samp[i]==(uint8_t)(expected++ % 251));
 }
 assert(capturevalue==expected+s.captureused-s.captureoffset);
 s.captureused=s.captureoffset=0;
 capturefailafter=(int)capturecalls+1;
 assert(usbhost_audio_transfer(&s,&b)==-EIO && b.nbytes<=196);
 puts("USB_CAPTURE_JITTER_ZERO_SHORT_PACKETS_AND_PARTIAL_ERROR_PASS");
}
int main(void)
{
 run(48000,1024,192000,true);
 run(48000,3072,192000,true);
 run(44100,4000,176400,true);
 run(48000,1000,1000,true);
 run(48000,1000,1000,false);
 capture_regression();
 puts("USB_PLAYBACK_PACKET_BOUNDARY_PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="usb-packet-test-") as tmp:
    root = pathlib.Path(tmp)
    unit, binary = root / "packet.c", root / "packet"
    unit.write_text(stub + function + tests)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", str(unit), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
