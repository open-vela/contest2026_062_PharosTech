#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the real DSP player against bounded fake audio devices and a temp WAV."""
import argparse, subprocess, tempfile, wave, struct, math
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('core',type=Path);p.add_argument('cjson',type=Path);a=p.parse_args();core=a.core.resolve();dsp=core.parent/'nyabula_dsp'
with tempfile.TemporaryDirectory(prefix='ny-dsp-player-') as temp:
 root=Path(temp);(root/'nuttx').mkdir();(root/'netutils').mkdir()
 (root/'nuttx/config.h').write_text('#define CONFIG_NYABULA_CORE_DSP 1\n#define CONFIG_NYABULA_CORE_BT 1\n#define CONFIG_NYABULA_CORE_MEDIA_ROOT "'+str(root)+'"\n#define CONFIG_NYABULA_CORE_MEDIA_DEVICE "satellite"\n#define CONFIG_NYABULA_CORE_DSP_BASS_DEVICE "bass"\n')
 (root/'nuttx/mutex.h').write_text('#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n#define nxmutex_lock(m) pthread_mutex_lock(m)\n#define nxmutex_unlock(m) pthread_mutex_unlock(m)\n')
 (root/'netutils/cJSON.h').symlink_to((a.cjson/'cJSON.h').resolve())
 with wave.open(str(root/'test.wav'),'wb') as w:
  w.setparams((2,2,48000,0,'NONE','not compressed'));w.writeframes(b''.join(struct.pack('<hh',int(10000*math.sin(i*2*math.pi*80/48000)),int(10000*math.sin(i*2*math.pi*80/48000))) for i in range(48000)))
 source=root/'test.c';source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
static size_t test_strlcpy(char*d,const char*s,size_t n){size_t l=strlen(s);if(n)snprintf(d,n,"%s",s);return l;}
#define strlcpy test_strlcpy
#include "ny_product_dsp.c"
#include "ny_product_store.c"
#include "ny_dsp_output.c"
struct ny_audio_stream_s {int bass;struct ny_dsp_output_s *dsp;};
static struct ny_audio_stream_s common;
static struct ny_audio_stream_s devices[2];static size_t written[2];static int fail_save,fail_bass,slow_save;static char *saved;static char *presets;
int ny_state_read(const char *key,char **value,size_t *length,size_t limit){assert(!strcmp(key,"product/v1/dsp")||!strcmp(key,"product/v1/dsp-presets"));char *stored=!strcmp(key,"product/v1/dsp")?saved:presets;if(!stored)return -ENOENT;*length=strlen(stored);assert(*length<=limit);*value=strdup(stored);return *value?0:-ENOMEM;}
int ny_state_write(const char *key,const void *value,size_t length){assert(!strcmp(key,"product/v1/dsp")||!strcmp(key,"product/v1/dsp-presets"));if(fail_save)return -EIO;if(slow_save){uint64_t before;nxmutex_lock(&g_dsp_lock);before=g_dsp_frames;nxmutex_unlock(&g_dsp_lock);usleep(100000);nxmutex_lock(&g_dsp_lock);assert(g_dsp_frames>before+1024);nxmutex_unlock(&g_dsp_lock);puts("DSP_SLOW_STORAGE_DOES_NOT_BLOCK_AUDIO_PASS");}char *copy=strndup(value,length);if(!copy)return -ENOMEM;char **slot=!strcmp(key,"product/v1/dsp")?&saved:&presets;free(*slot);*slot=copy;return 0;}
int ny_audio_stream_open(struct ny_audio_stream_s **s,const struct ny_audio_stream_config_s*c){if(c->dsp_source){int ret=ny_dsp_output_open(&common.dsp,c);if(!ret)*s=&common;return ret;}int b=!strcmp(c->path,"bass");if(b&&fail_bass)return -ENODEV;devices[b].bass=b;*s=&devices[b];return 0;}
ssize_t ny_audio_stream_write(struct ny_audio_stream_s*s,const void*p,size_t n,int timeout){if(s->dsp)return ny_dsp_output_write(s->dsp,p,n,timeout);written[s->bass]+=n;usleep(1000);return n;}
void ny_audio_stream_close(struct ny_audio_stream_s*s){if(s&&s->dsp){ny_dsp_output_close(s->dsp);s->dsp=NULL;}}
int ny_audio_stream_drain(struct ny_audio_stream_s*s,int timeout){return s->dsp?ny_dsp_output_drain(s->dsp,timeout):0;}
int ny_audio_stream_unity(struct ny_audio_stream_s*s){return 0;}
int ny_audio_stream_pause(struct ny_audio_stream_s*s,bool paused){return 0;}
size_t ny_audio_stream_queued(struct ny_audio_stream_s*s){if(!s)return 0;return s->dsp?ny_dsp_output_queued(s->dsp):2048;}
void ny_audio_stream_stats(struct ny_audio_stream_s*s,struct ny_audio_stream_stats_s*v){memset(v,0,sizeof(*v));}
static atomic_bool call_active;
static int claims;
int ny_product_bt_speaker_claim(bool alert){claims++;return 0;}
void ny_product_bt_speaker_release(void){assert(claims>0);claims--;}
bool ny_product_bt_call_active(void){return atomic_load(&call_active);}
static int call(const char*t,const char*j,cJSON**r){struct ny_product_caller_s c={"owner",NY_PRODUCT_OWNER,true};cJSON*d=cJSON_Parse(j);*r=NULL;int x=ny_product_dsp_request(&c,t,d,r);cJSON_Delete(d);return x;}
static bool running(void){bool r;nxmutex_lock(&g_dsp_lock);r=g_dsp_running;nxmutex_unlock(&g_dsp_lock);return r;}
static void eq_switch_proof(void){
 struct ny_product_dsp_settings_s cfg=g_dsp_settings;
 float input[512],output[768];double power[2]={0};
 ny_dsp_reset_eq(&cfg);cfg.bass=false;cfg.filter.bypass=false;
 cfg.filter.master_gain_db=-12;cfg.filter.output[0].gain_db=-3;
 cfg.input[8].gain_db=6;
 for(int enabled=0;enabled<2;enabled++){
  cfg.eq_enabled=enabled;struct nyadsp_s *d=ny_dsp_output_create(&cfg);assert(d);
  for(int block=0;block<40;block++){
   for(int i=0;i<256;i++)input[i*2]=input[i*2+1]=.01f*sinf((block*256+i)*2*3.141592653589793*1200/48000);
   assert(nyadsp_process(d,input,output,256)==0);
   if(block>30)for(int i=0;i<256;i++)power[enabled]+=output[3*i]*output[3*i];
  }
  nyadsp_destroy(d);
 }
 assert(fabs(10*log10(power[1]/power[0])-6)<.05);
 assert(fabs(10*log10(power[0]/(9*256*.01*.01/2))+15)<.05);
 puts("EQ_SWITCH_PRESERVES_MASTER_AND_OUTPUT_GAIN_PASS");
}
int main(void){cJSON*r;
 assert(call("dsp.status","{}",&r)==0);assert(!cJSON_IsTrue(cJSON_GetObjectItem(r,"running")));
 cJSON *cfg=cJSON_GetObjectItem(r,"config"), *bank=cJSON_GetObjectItem(cfg,"eqBands16");
 assert(cJSON_GetArraySize(bank)==16);assert(cJSON_GetArraySize(cJSON_GetObjectItem(cfg,"eqBands"))==4);
 cJSON_SetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(bank,15),"gainDb"),4);
 char *expanded=cJSON_PrintUnformatted(cfg);cJSON_Delete(r);
 assert(call("dsp.config",expanded,&r)==0);free(expanded);cJSON_Delete(r);assert(g_dsp_settings.input[15].gain_db==4);
 assert(call("dsp.config","{\"eqBands16\":[]}",&r)==-EINVAL);
 const int crossovers[]={40,120,250,300,500};
 for(size_t i=0;i<sizeof(crossovers)/sizeof(crossovers[0]);i++){
  char data[80];snprintf(data,sizeof(data),"{\"crossoverHz\":%d,\"persist\":false}",crossovers[i]);
  assert(call("dsp.config",data,&r)==0);cJSON_Delete(r);
  assert(g_dsp_settings.filter.crossover_hz==crossovers[i]);
 }
 assert(call("dsp.config","{\"crossoverHz\":39}",&r)==-EINVAL);
 assert(call("dsp.config","{\"crossoverHz\":501}",&r)==-EINVAL);
 assert(g_dsp_settings.filter.crossover_hz==500);
 assert(call("dsp.config","{\"restore\":true}",&r)==0);cJSON_Delete(r);
 assert(g_dsp_settings.filter.crossover_hz==120);
 puts("PRODUCT_DSP_CROSSOVER_40_500_BOUNDARY_PASS");
 char *old_saved=strdup(saved);
 assert(call("dsp.config","{\"satelliteHighpassHz\":250,\"bassLowpassHz\":500}",&r)==0);cJSON_Delete(r);
 assert(g_dsp_settings.crossover.satellite_highpass_hz==250 && g_dsp_settings.crossover.bass_lowpass_hz==500);
 assert(g_dsp_settings.filter.crossover_hz==250);
 g_dsp_loaded=false;assert(call("dsp.status","{}",&r)==0);cJSON_Delete(r);
 assert(g_dsp_settings.crossover.satellite_highpass_hz==250 && g_dsp_settings.crossover.bass_lowpass_hz==500);
 assert(call("dsp.config","{\"bassLowpassHz\":450,\"persist\":false}",&r)==0);cJSON_Delete(r);
 assert(g_dsp_settings.crossover.satellite_highpass_hz==250 && g_dsp_settings.crossover.bass_lowpass_hz==450);
 assert(call("dsp.config","{\"bassLowpassHz\":501}",&r)==-EINVAL);
 assert(call("dsp.config","{\"satelliteHighpassHz\":null}",&r)==-EINVAL);
 assert(g_dsp_settings.crossover.bass_lowpass_hz==450);
 assert(call("dsp.config","{\"restore\":true}",&r)==0);cJSON_Delete(r);
 assert(g_dsp_settings.crossover.bass_lowpass_hz==500);
 assert(call("dsp.config","{\"crossoverHz\":200,\"persist\":false}",&r)==0);cJSON_Delete(r);
 assert(g_dsp_settings.crossover.satellite_highpass_hz==200 && g_dsp_settings.crossover.bass_lowpass_hz==200);
 cJSON *legacy=cJSON_Parse(old_saved);assert(legacy);
 cJSON_DeleteItemFromObjectCaseSensitive(legacy,"satelliteHighpassHz");
 cJSON_DeleteItemFromObjectCaseSensitive(legacy,"bassLowpassHz");
 free(saved);saved=cJSON_PrintUnformatted(legacy);cJSON_Delete(legacy);g_dsp_loaded=false;
 assert(call("dsp.status","{}",&r)==0);cJSON_Delete(r);
 assert(g_dsp_settings.crossover.satellite_highpass_hz==120 && g_dsp_settings.crossover.bass_lowpass_hz==120);
 free(saved);saved=old_saved;g_dsp_loaded=false;
 assert(call("dsp.status","{}",&r)==0);cJSON_Delete(r);
 puts("PRODUCT_DSP_SPLIT_PERSIST_PREVIEW_RESTORE_LEGACY_PASS");

 assert(g_dsp_settings.eq_enabled);eq_switch_proof();
 assert(call("dsp.preset.save","{\"name\":\"custom\"}",&r)==0);cJSON_Delete(r);
 assert(call("dsp.preset.save","{\"name\":\"custom\"}",&r)==-EEXIST);
 assert(call("dsp.preset.save","{\"name\":\"custom\",\"overwrite\":true}",&r)==0);cJSON_Delete(r);
 char *bankcopy=strdup(presets);
 assert(call("dsp.config","{\"eqEnabled\":false}",&r)==0);cJSON_Delete(r);
 assert(!g_dsp_settings.eq_enabled && g_dsp_settings.input[15].gain_db==4);
 g_dsp_loaded=false;assert(call("dsp.status","{}",&r)==0);cJSON_Delete(r);assert(!g_dsp_settings.eq_enabled);
 assert(call("dsp.config","{\"resetEq\":true}",&r)==0);cJSON_Delete(r);
 assert(g_dsp_settings.eq_enabled && g_dsp_settings.input[15].gain_db==0 && g_dsp_settings.filter.master_gain_db==-12);
 assert(!strcmp(bankcopy,presets));free(bankcopy);
 assert(call("dsp.presets","{}",&r)==0);
 cJSON *savedcfg=cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(r,"items"),0),"config");
 assert(cJSON_GetObjectItem(savedcfg,"bassEnabled")==NULL);
 assert(cJSON_GetObjectItem(savedcfg,"satelliteHighpassHz")==NULL);
 assert(cJSON_GetObjectItem(savedcfg,"bassLowpassHz")==NULL);
 char *restorecfg=cJSON_PrintUnformatted(savedcfg);cJSON_Delete(r);
 assert(call("dsp.config",restorecfg,&r)==0);cJSON_Delete(r);free(restorecfg);assert(g_dsp_settings.input[15].gain_db==4);
 fail_save=1;assert(call("dsp.preset.delete","{\"name\":\"custom\"}",&r)==-EIO);fail_save=0;
 assert(call("dsp.preset.delete","{\"name\":\"custom\"}",&r)==0);cJSON_Delete(r);
 assert(call("dsp.preset.delete","{\"name\":\"custom\"}",&r)==-ENOENT);
 assert(call("dsp.preset.save","{\"name\":\"\"}",&r)==-EINVAL);
 for(int i=0;i<8;i++){char data[64];snprintf(data,sizeof(data),"{\"name\":\"preset%d\"}",i);assert(call("dsp.preset.save",data,&r)==0);cJSON_Delete(r);}
 assert(call("dsp.preset.save","{\"name\":\"overflow\"}",&r)==-ENOSPC);


 assert(call("dsp.config","{\"eqDb\":[6,0,-3,2],\"bassEnabled\":true}",&r)==0);cJSON_Delete(r);
 assert(call("dsp.config","{\"eqDb\":[99,0,0,0]}",&r)==-EINVAL);assert(g_dsp_settings.input[2].gain_db==6);
 assert(call("dsp.config","{\"outputGainDb\":[-30,0,0]}",&r)==-EINVAL);
 char *persisted=strdup(saved);
 assert(call("dsp.config","{\"persist\":false,\"masterDb\":-9}",&r)==0);cJSON_Delete(r);
 assert(g_dsp_settings.filter.master_gain_db==-9 && !g_dsp_settings_saved && !strcmp(saved,persisted));
 assert(call("dsp.config","{\"restore\":true}",&r)==0);cJSON_Delete(r);assert(g_dsp_settings.filter.master_gain_db==-12 && g_dsp_settings_saved);free(persisted);
 fail_save=1;assert(call("dsp.config","{\"masterDb\":-6}",&r)==-EIO);assert(g_dsp_settings.filter.master_gain_db==-12);fail_save=0;
 assert(call("dsp.play","{\"track\":\"../escape\"}",&r)==-EINVAL);
 assert(call("dsp.play","{\"track\":\"test.wav\"}",&r)==0);cJSON_Delete(r);
 assert(g_dsp_requested && !g_dsp_joinable);assert(ny_product_dsp_tick()==0);
 usleep(10000);
 assert(call("dsp.config","{\"bassEnabled\":false,\"persist\":false}",&r)==0);cJSON_Delete(r);
 usleep(30000);
 assert(call("dsp.config","{\"bassEnabled\":true,\"persist\":false}",&r)==0);cJSON_Delete(r);
 slow_save=1;
 assert(call("dsp.config","{\"masterDb\":-12}",&r)==0);cJSON_Delete(r);slow_save=0;
 puts("DSP_HOT_BASS_TOGGLE_PASS");
 assert(call("dsp.config","{\"satelliteHighpassHz\":250,\"bassLowpassHz\":500,\"persist\":false}",&r)==0);cJSON_Delete(r);
 usleep(10000);assert(call("dsp.config","{\"eqDb\":[0,2,0,0]}",&r)==0);cJSON_Delete(r);
 while(running())usleep(10000);ny_product_dsp_shutdown();assert(g_dsp_error==0);assert(g_dsp_frames>=48000);assert(written[0]>=48000*4);assert(written[1]>0);
 fail_bass=1;assert(call("dsp.play","{\"track\":\"test.wav\"}",&r)==0);cJSON_Delete(r);assert(ny_product_dsp_tick()==0);while(running())usleep(10000);ny_product_dsp_shutdown();assert(g_dsp_error==-ENODEV);
 fail_bass=0;assert(call("dsp.play","{\"track\":\"test.wav\"}",&r)==0);cJSON_Delete(r);assert(ny_product_dsp_tick()==0);usleep(10000);atomic_store(&call_active,true);while(running())usleep(10000);ny_product_dsp_shutdown();assert(g_dsp_error==-EINTR);assert(claims==0);
 free(presets);free(saved);puts("NY_DSP_PLAYER_PASS");return 0;}
''')
 for flags in [['-O2'],['-O1','-g','-fsanitize=address,undefined']]:
  binary=root/'test';subprocess.run(['cc','-std=c11','-D_GNU_SOURCE',*flags,'-I'+str(root),'-I'+str(core),'-I'+str(dsp),str(source),str(a.cjson/'cJSON.c'),str(dsp/'nyadsp.c'),str(dsp/'nyadsp_clock.c'),str(dsp/'nyadsp_resample.c'),'-pthread','-lm','-o',str(binary)],check=True);subprocess.run([str(binary)],check=True)
