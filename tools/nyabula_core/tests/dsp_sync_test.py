#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run production dual-output pacing against independently clocked bounded sinks."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p=argparse.ArgumentParser()
p.add_argument('core',type=Path)
p.add_argument('cjson',type=Path)
a=p.parse_args(); core=a.core.resolve(); dsp=core.parent/'nyabula_dsp'
with tempfile.TemporaryDirectory(prefix='ny-dsp-sync-') as temp:
 root=Path(temp); (root/'nuttx').mkdir(); (root/'netutils').mkdir()
 (root/'nuttx/config.h').write_text('#define CONFIG_NYABULA_CORE_DSP 1\n#define CONFIG_NYABULA_CORE_AUDIO 1\n#define CONFIG_NYABULA_CORE_DSP_BASS_DEVICE "bass"\n#define CONFIG_NYABULA_CORE_AUDIO_OUTPUT_DEVICE "satellite"\n#define CONFIG_NYABULA_CORE_AUDIO_INPUT_DEVICE "mic"\n')
 (root/'netutils/cJSON.h').symlink_to((a.cjson/'cJSON.h').resolve())
 source=root/'test.c'
 source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "ny_dsp_output.c"
struct ny_audio_stream_s {
 struct ny_audio_stream_config_s config;
 double end[32];
 unsigned head, count, partial;
 double rate;
 bool started;
};
static struct ny_audio_stream_s devices[2];
static struct ny_product_dsp_settings_s settings;
static double now, skew;
static unsigned volume=1000;
static unsigned raw_pauses;
static double power[2];
static unsigned samples[2];
unsigned int ny_product_audio_output_level(void){return volume;}
static void pump(struct ny_audio_stream_s*s){
 while(s->started && s->count && s->end[s->head]<=now+1e-10){s->head=(s->head+1)%32;s->count--;}
}
int ny_audio_stream_open(struct ny_audio_stream_s **s,const struct ny_audio_stream_config_s*c){
 int bass=!strcmp(c->path,"bass");*s=&devices[bass];memset(*s,0,sizeof(**s));(*s)->config=*c;(*s)->rate=48000*(bass?1+skew:1);return 0;
}
int ny_audio_stream_unity(struct ny_audio_stream_s*s){return 0;}
ssize_t ny_audio_stream_write(struct ny_audio_stream_s*s,const void*p,size_t bytes,int timeout){
 const int16_t*pcm=p;int b=s==&devices[1];
 for(size_t i=0;i<bytes/2;i++){power[b]+=(double)pcm[i]*pcm[i];samples[b]++;}
 size_t left=bytes;
 while(left){
  pump(s);
  if(!s->partial && s->count>=s->config.buffers){now=s->end[s->head];pump(s);}
  size_t n=s->config.chunk-s->partial;if(n>left)n=left;s->partial+=n;left-=n;
  if(s->partial==s->config.chunk){
   double tail=s->count?s->end[(s->head+s->count-1)%32]:now;
   if(tail<now)tail=now;
   s->end[(s->head+s->count)%32]=tail+s->config.chunk/4/s->rate;
   s->count++;s->partial=0;assert(s->count<=s->config.buffers);
   if(!s->started && s->count>=s->config.prefill){
    s->started=true;for(unsigned j=0;j<s->count;j++)s->end[(s->head+j)%32]=now+(j+1)*s->config.chunk/4/s->rate;
   }
  }
 }
 return bytes;
}
size_t ny_audio_stream_queued(struct ny_audio_stream_s*s){if(!s)return 0;pump(s);return s->count*s->config.chunk+s->partial;}
void ny_audio_stream_stats(struct ny_audio_stream_s*s,struct ny_audio_stream_stats_s*v){memset(v,0,sizeof(*v));}
void ny_audio_stream_close(struct ny_audio_stream_s*s){}
int ny_audio_stream_drain(struct ny_audio_stream_s*s,int timeout){return 0;}
int ny_audio_stream_pause(struct ny_audio_stream_s*s,bool paused){raw_pauses++;return -ENOTSUP;}
int ny_product_dsp_acquire(const char*source,struct ny_product_dsp_settings_s*c,uint32_t*r){*c=settings;*r=1;return 0;}
struct nyadsp_s *ny_product_dsp_take_update(struct ny_product_dsp_settings_s*c,uint32_t*r){*c=settings;*r=1;return NULL;}
void ny_product_dsp_release(int error){assert(!error);}
void ny_product_dsp_progress(size_t frames,float peak,double ppm,uint32_t starved,size_t sat,size_t bass){}
static double latency(struct ny_audio_stream_s*s){pump(s);return (s->count?s->end[(s->head+s->count-1)%32]-now:0)+s->partial/4/s->rate;}
int main(void){
 setvbuf(stdout,NULL,_IONBF,0);
 struct ny_audio_stream_config_s config={.path="satellite",.rate=48000,.channels=2};
 int16_t input[512];for(unsigned i=0;i<256;i++)input[i*2]=input[i*2+1]=lrint(1000*sin(i*2*3.141592653589793/256));
 nyadsp_default_config(&settings.filter);settings.filter.master_gain_db=0;settings.filter.bypass=false;
 for(unsigned i=0;i<NYADSP_INPUT_EQ_BANDS;i++)settings.input[i]=settings.filter.eq[0];
 settings.crossover.satellite_highpass_hz=155;settings.crossover.bass_lowpass_hz=240;settings.bass=true;
 for(int mode=0;mode<2;mode++)for(int sign=-1;sign<=1;sign++){
  struct ny_dsp_output_s*out;now=0;skew=sign*.0015;volume=1000;
  assert(ny_dsp_output_open(&out,&config)==0);
  double maximum=0,late=0,source_pause=0;
  for(unsigned block=0;block<60000;block++){
   if(block==20000){
    assert(ny_dsp_output_pause(out,true)==0);now+=.5;source_pause=.5;
    assert(ny_dsp_output_pause(out,false)==0);assert(raw_pauses==0);
   }
   if(mode && now<(block+1)*256.0/48000+source_pause)now=(block+1)*256.0/48000+source_pause;
   assert(ny_dsp_output_write(out,input,sizeof(input),100)==sizeof(input));
   double difference=fabs(latency(&devices[0])-latency(&devices[1]));
   if(block>1000 && difference>maximum)maximum=difference;
   if(block>59000 && difference>late)late=difference;
  }
  printf("DUAL_CLOCK_LONG_PLAY source_paced=%d ppm=%d seconds=%.3f max_offset_ms=%.3f late_offset_ms=%.3f correction_ppm=%.3f\n",mode,sign*1500,now,maximum*1000,late*1000,(out->clock.ratio-1)*1e6);
  assert(maximum<.016);assert(late<.016);
  // The USB output follows global volume without touching satellite PCM.
  double rms[2][2];
  for(unsigned level=0;level<2;level++){
   volume=level?250:500;
   for(unsigned block=0;block<20;block++)ny_dsp_output_write(out,input,sizeof(input),100);
   memset(power,0,sizeof(power));memset(samples,0,sizeof(samples));
   for(unsigned block=0;block<100;block++)ny_dsp_output_write(out,input,sizeof(input),100);
   for(int b=0;b<2;b++)rms[level][b]=sqrt(power[b]/samples[b]);
  }
  assert(fabs(rms[1][1]/rms[0][1]-.5)<.01);
  assert(fabs(rms[1][0]/rms[0][0]-1)<.001);
  volume=0;memset(power,0,sizeof(power));
  for(unsigned block=0;block<20;block++)ny_dsp_output_write(out,input,sizeof(input),100);
  assert(power[1]==0);assert(power[0]>0);
  ny_dsp_output_close(out);
 }
 puts("DSP_DUAL_CLOCK_AND_GLOBAL_VOLUME_PASS");return 0;
}
''')
 binary=root/'test'
 subprocess.run(['cc','-std=c11','-O2','-I'+str(root),'-I'+str(core),'-I'+str(dsp),str(source),str(dsp/'nyadsp.c'),str(dsp/'nyadsp_clock.c'),str(dsp/'nyadsp_resample.c'),'-lm','-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=180)
