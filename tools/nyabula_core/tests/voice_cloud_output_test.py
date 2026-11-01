#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise actual cloud-speech scheduling against network gaps and a 40ms sink."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('core', type=Path)
parser.add_argument('--dual', action='store_true')
parser.add_argument('--tick', action='store_true')
args = parser.parse_args()
source = (args.core/'ny_voice.c').read_text(encoding='utf-8')

def function(name, optional=False):
    match = re.search(r'static (?:int|void\s*\*)\s*'+name+r'\([^;]*?\n\{', source)
    if match is None and optional:
        return ''
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

structs = '\n'.join(re.findall(r'struct ny_voice_mimo(?:_chunk)?_s\n\{.*?\n\};', source, re.S))
defines = '\n'.join(re.findall(r'^#define NY_VOICE_(?:OUT_CHUNK|PLAY_RESERVE_MS|PLAY_POLL_MS|SENTENCE_MAX|MIMO_\w+)\s+\S+', source, re.M))
code = r'''
#define _GNU_SOURCE
#define CONFIG_NYABULA_CORE_DSP 1
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ny_voice_dsp.h"
#define NY_VOICE_OUT_RATE 44100U
#define NY_VOICE_OUT_CHANNELS 2
#define NY_MIMO_AUDIO_RATE 24000U
#define TEST_BLOCK 512
struct ny_voice_audio_s { double queued, last; bool started; unsigned starved; };
static int16_t g_voice_out[4096], collected[65536], generated[65536];
static size_t collected_count, generated_count;
static pthread_t owner;
static atomic_bool cancelled;
static unsigned calls;
static atomic_bool first_written;
static size_t pending_silence, interior_silence;
static int mode;
static double now(void) {
 struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
 return (double)t.tv_sec+(double)t.tv_nsec/1e9;
}
static void drain(struct ny_voice_audio_s *audio) {
 double t=now(), used=(t-audio->last)*NY_VOICE_OUT_RATE*4;
 if(audio->started && used>audio->queued) {audio->starved++;audio->queued=0;}
 else if(audio->started) audio->queued-=used;
 audio->last=t;
}
static size_t ny_voice_audio_queued(struct ny_voice_audio_s *audio) {
 drain(audio);return (size_t)audio->queued;
}
static ssize_t ny_voice_audio_write(struct ny_voice_audio_s *audio,
 const void *pcm,size_t bytes,int timeout) {
 (void)timeout;
 if(pcm != g_voice_out && collected_count)pending_silence+=bytes;
 if(pcm == g_voice_out){interior_silence+=pending_silence;pending_silence=0;}
 assert(pthread_equal(owner,pthread_self()));
 if(mode==3)return -EPIPE;
 size_t done=0;double capacity=NY_VOICE_OUT_RATE*4*.04;
 while(done<bytes) {
  drain(audio);
  size_t room=(size_t)(capacity-audio->queued);
  if(!room){usleep(1000);continue;}
  size_t take=bytes-done;if(take>room)take=room;
  audio->queued+=take;audio->started=true;done+=take;
 }
 if(pcm == g_voice_out)atomic_store(&first_written,true);
 return (ssize_t)bytes;
}
static bool ny_voice_play_cancelled(void){return atomic_load(&cancelled);}
static int ny_voice_play_write(struct ny_voice_audio_s *audio,size_t bytes) {
 assert(collected_count+bytes/2<sizeof(collected)/sizeof(collected[0]));
 memcpy(collected+collected_count,g_voice_out,bytes);collected_count+=bytes/2;
 ssize_t ret=ny_voice_audio_write(audio,g_voice_out,bytes,100);
 if(mode==2 && collected_count>2000)atomic_store(&cancelled,true);
 return ret<0?(int)ret:ret==(ssize_t)bytes?0:-EIO;
}
static int ny_mimo_speak(const char *text,
 int (*callback)(void*,const int16_t*,size_t),void *context) {
 (void)text;calls++;
 if(mode==1){usleep(20000);return -EIO;}
 if(mode==0)usleep(120000);
 for(unsigned block=0;block<(mode && mode!=4?32U:4U);block++) {
  if(mode==0 && block==1)usleep(80000);
  if(mode==4 && block==1){while(!atomic_load(&first_written))usleep(100);usleep(2000);}
  int ret=callback(context,NULL,0);if(ret)return ret;
  int16_t pcm[TEST_BLOCK];
  for(size_t i=0;i<TEST_BLOCK;i++) {
   pcm[i]=(int16_t)(8000*sin((double)(generated_count+i)*(mode==4?2*3.141592653589793*80/24000:.13)));
  }
  assert(generated_count+TEST_BLOCK<sizeof(generated)/sizeof(generated[0]));
  memcpy(generated+generated_count,pcm,sizeof(pcm));generated_count+=TEST_BLOCK;
  ret=callback(context,pcm,TEST_BLOCK);if(ret)return ret;
 }
 return 0;
}
'''
main = r'''
int main(void) {
 owner=pthread_self();
 for(mode=0;mode<5;mode++) {
  struct ny_voice_audio_s audio={.last=now()};
  atomic_store(&cancelled,false);calls=0;collected_count=generated_count=0;
  atomic_store(&first_written,false);pending_silence=interior_silence=0;
  int ret=ny_voice_mimo_speech(&audio,"One sentence. Another sentence. Last sentence.");
  if(mode==0) {
   assert(ret==0 && calls==3 && generated_count==3*4*TEST_BLOCK);
   struct ny_voice_resample_s resample={0};
   assert(ny_voice_resample_rate_init(&resample,24000,44100)==0);
   size_t at=0;
   for(size_t offset=0;offset<generated_count;offset+=TEST_BLOCK) {
    float input[TEST_BLOCK], output[1024];int16_t pcm[2048];
    ny_voice_s16_to_f32(input,generated+offset,TEST_BLOCK);
    size_t count=ny_voice_resample(&resample,input,TEST_BLOCK,output,1024);
    ny_voice_f32_to_s16_stereo(pcm,output,count);
    assert(!memcmp(pcm,collected+at,count*4));at+=count*2;
   }
   assert(at==collected_count);
   printf("cloud network gaps: starved=%u, ordered PCM samples=%zu\n",audio.starved,at);
   fflush(stdout);assert(audio.starved==0);
  } else if(mode==1)assert(ret==-EIO);
  else if(mode==2)assert(ret==-ECANCELED);
  else if(mode==3)assert(ret==-EPIPE);
  else {printf("short 2ms gap: interior silence bytes=%zu (%.2f ms)\n",interior_silence,interior_silence/176.4);fflush(stdout);assert(ret==0);assert(interior_silence==0);}
 }
 puts("VOICE_CLOUD_OUTPUT_PASS gaps ordered_pcm cancellation output_error backend_error");
}
'''
if args.dual:
    import ast
    sync = (Path(__file__).parent/'dsp_sync_test.py').read_text()
    tree = ast.parse(sync)
    fixture = next(ast.literal_eval(n.args[0]) for n in ast.walk(tree)
                   if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute)
                   and n.func.attr == 'write_text' and n.args
                   and isinstance(n.args[0], ast.Constant)
                   and 'struct ny_audio_stream_s {' in str(n.args[0].value))
    fixture = fixture[fixture.index('struct ny_audio_stream_s {'):fixture.index('static double latency')]
    fixture = fixture.replace('now', 'sink_now')
    fixture = fixture.replace('static void pump(struct ny_audio_stream_s*s){',
        'static void pump(struct ny_audio_stream_s*s){sink_now=now();')
    fixture = fixture.replace('if(!s->partial && s->count>=s->config.buffers){sink_now=s->end[s->head];pump(s);}',
        'while(!s->partial && s->count>=s->config.buffers){usleep(200);pump(s);}')
    fixture = fixture.replace('assert(!error);','(void)error;')
    fixture = fixture.replace('static unsigned samples[2];', 'static unsigned samples[2];\nstatic int peaks[2], steps[2], previous[2];')
    fixture = fixture.replace('power[b]+=(double)pcm[i]*pcm[i];samples[b]++;',
        'power[b]+=(double)pcm[i]*pcm[i];samples[b]++;if(abs(pcm[i])>peaks[b])peaks[b]=abs(pcm[i]);if(abs(pcm[i]-previous[b])>steps[b])steps[b]=abs(pcm[i]-previous[b]);previous[b]=pcm[i];')
    # Replace only the device-facing layer; use real production SRC, EQ,
    # crossover, clock correction and both output streams.
    code = code.replace('struct ny_voice_audio_s { double queued, last; bool started; unsigned starved; };',
        'struct ny_voice_audio_s { double queued, last; bool started; unsigned starved; struct ny_dsp_output_s *out; };')
    at = code.index('static void drain(')
    code = code[:at] + '#include "ny_dsp_output.c"\n' + fixture + code[at:]
    code = code.replace('drain(audio);return (size_t)audio->queued;', 'return ny_dsp_output_queued(audio->out);')
    begin=code.index(' size_t done=0;double capacity=')
    end=code.index(' if(pcm == g_voice_out)',begin)
    code=code[:begin]+' ssize_t result=ny_dsp_output_write(audio->out,pcm,bytes,timeout);if(result<0)return result;\n'+code[end:]
    main=main.replace('for(mode=0;mode<5;mode++)', 'for(mode=0;mode<5;mode+=4)')
    main=main.replace('cloud network gaps: starved=%u, ordered PCM samples=%zu',
                      'DSP fixture (starvation not modeled=%u): ordered PCM samples=%zu')
    main=main.replace('int ret=ny_voice_mimo_speech', r'''nyadsp_default_config(&settings.filter);
  settings.filter.master_gain_db=-18;settings.filter.crossover_hz=220;
  settings.filter.output[0].gain_db=settings.filter.output[1].gain_db=5;
  settings.crossover.satellite_highpass_hz=settings.crossover.bass_lowpass_hz=220;
  settings.bass=true;settings.eq_enabled=true;
  float hz[16]={30,56,73,125,249,303,487,800,1200,2369,2809,4469,4730,9901,12666,15020};
  float db[16]={3.5,7,5,2,7,1,3.5,-.5,-1,-1.5,1.5,2.5,1.5,.5,2.5,3};
  for(int i=0;i<16;i++){settings.input[i]=(struct nyadsp_eq_s){hz[i],1.4f,db[i]};settings.enabled[i]=true;settings.kinds[i]=NYADSP_FILTER_PEAK;}
  struct ny_audio_stream_config_s cfg={.path="satellite",.rate=44100,.channels=2};
  memset(peaks,0,sizeof(peaks));memset(steps,0,sizeof(steps));memset(previous,0,sizeof(previous));
  assert(ny_dsp_output_open(&audio.out,&cfg)==0);
  int ret=ny_voice_mimo_speech''')
    main=main.replace(' }\n puts(', r'''  assert(ny_dsp_output_drain(audio.out,1000)==0);
  printf("DSP PCM mode=%d satellite_peak=%d bass_peak=%d satellite_step=%d bass_step=%d\n",mode,peaks[0],peaks[1],steps[0],steps[1]);
  assert(peaks[0]<32767 && peaks[1]<32767);
  ny_dsp_output_close(audio.out);
 }'''+'\n puts(')

if args.tick:
    code += r'''
static int ny_test_tick_wait(sem_t *sem, const struct timespec *deadline)
{
 struct timespec rounded=*deadline;
 rounded.tv_nsec+=10000000L;
 if(rounded.tv_nsec>=1000000000L){rounded.tv_sec++;rounded.tv_nsec-=1000000000L;}
 return sem_timedwait(sem,&rounded);
}
#define sem_timedwait ny_test_tick_wait
'''

body='\n'.join(function(n,optional=True) for n in ['ny_voice_play_keepalive','ny_voice_mimo_enqueue','ny_voice_mimo_produce'])
body+='\n'+function('ny_voice_mimo_pcm')+'\n'+function('ny_voice_mimo_speech')
with tempfile.TemporaryDirectory(prefix='ny-cloud-output-') as directory:
    p=Path(directory);extra=[]
    if args.dual:
        (p/'nuttx').mkdir();(p/'netutils').mkdir()
        (p/'nuttx/config.h').write_text('#define CONFIG_NYABULA_CORE_DSP 1\n#define CONFIG_NYABULA_CORE_AUDIO 1\n#define CONFIG_NYABULA_CORE_DSP_BASS_DEVICE \"bass\"\n#define CONFIG_NYABULA_CORE_AUDIO_OUTPUT_DEVICE \"satellite\"\n')
        (p/'netutils/cJSON.h').write_text('typedef struct cJSON cJSON;\n')
        dsp=args.core.parent/'nyabula_dsp'
        extra=['-Wno-unused-parameter','-Wno-sign-compare','-I'+str(p),'-I'+str(dsp),str(dsp/'nyadsp.c'),str(dsp/'nyadsp_clock.c'),str(dsp/'nyadsp_resample.c')]
    c=p/'test.c';c.write_text(code+'\n'+defines+'\n'+structs+'\n'+body+main)
    for name,flags in [('plain',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        binary=p/name
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',*flags,*extra,'-I'+str(args.core),str(c),str(args.core/'ny_voice_dsp.c'),'-pthread','-lm','-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=20)
