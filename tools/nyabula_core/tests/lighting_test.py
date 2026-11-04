#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile the real four-zone renderer and product handler with bounded I/O fixtures."""
import argparse, subprocess, tempfile, re
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('core',type=Path);p.add_argument('cjson',type=Path);args=p.parse_args()
store_source=(args.core/'ny_product_store.c').read_text()
store_key=re.search(r"static int ny_product_store_key\([^;]+?\n\{.*?\n\}",store_source,re.S)
assert store_key
code=r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int calls, failures, ticks;
static uint64_t now=10000;
static unsigned int sent[33];
int test_open(const char *path,int flags,...){(void)path;(void)flags;return 5;}
ssize_t test_write(int fd,const void *bytes,size_t count){assert(fd==5&&count==132);calls++;if(failures){failures--;errno=EIO;return -1;}memcpy(sent,bytes,count);return count;}
int test_close(int fd){assert(fd==5);return 0;}
int sched_lock(void){return 0;} int sched_unlock(void){return 0;}
int test_usleep(useconds_t usec);
#define open test_open
#define write test_write
#define close test_close
#define usleep test_usleep
#include "ny_product_lighting.c"
#undef open
#undef write
#undef close
#undef usleep
static cJSON *saved;
static uint64_t saved_revision;
static int save_error;
int test_usleep(useconds_t usec){now+=usec/1000;if(++ticks==12)g_lighting_stopping=true;return 0;}
uint64_t ny_product_time_ms(bool monotonic){(void)monotonic;return now;}
int ny_product_store_read(const char *domain,cJSON **out,uint64_t *rev){assert(!strcmp(domain,"lighting"));*out=saved?cJSON_Duplicate(saved,true):NULL;*rev=saved_revision;return 0;}
int ny_product_store_write(const char *domain,const cJSON *value,uint64_t expected,uint64_t *revision){assert(!strcmp(domain,"lighting"));if(save_error)return save_error;if(expected!=saved_revision)return -ESTALE;cJSON_Delete(saved);saved=cJSON_Duplicate(value,true);*revision=++saved_revision;return 0;}
"""+store_key.group()+r"""
static bool black(const uint32_t *pixels){for(int i=0;i<33;i++)if(pixels[i])return false;return true;}
static int call(enum ny_product_role_e role,const char *topic,const char *json){struct ny_product_caller_s caller={"test",role,true};cJSON *in=cJSON_Parse(json),*out=NULL;assert(in);int ret=ny_product_lighting_request(&caller,topic,in,&out);cJSON_Delete(in);cJSON_Delete(out);return ret;}
int main(void){
 char key[64];assert(ny_product_store_key("lighting",key,sizeof(key))==0);assert(!strcmp(key,"product/v1/lighting"));assert(ny_product_store_key("lighting-arbitrary",key,sizeof(key))==-EINVAL);
 struct ny_lighting_group_s groups[4];uint32_t pixels[33],other[33];
 struct ny_lighting_input_s input={.iris={0xff0000,0x0000ff}};
 ny_lighting_defaults(groups);ny_lighting_render(groups,&input,1234,pixels);assert(black(pixels));
 assert(ny_lighting_complement(0xff0000)==0x00ffff);assert(ny_lighting_complement(0)==0);assert(ny_lighting_complement(0x808080)==0x808080);
 groups[0].enabled=groups[1].enabled=true;
 ny_lighting_render(groups,&input,0,pixels);assert(pixels[0]==0x1f0000&&pixels[1]==0x00001f);
 groups[0].color_mode=NY_LIGHTING_COMPLEMENT;ny_lighting_render(groups,&input,0,pixels);assert(pixels[0]==0x001f1f);
 ny_lighting_defaults(groups);groups[2].enabled=true;groups[2].color=0xffffff;groups[2].brightness=25;
 ny_lighting_render(groups,&input,0,pixels);for(int i=0;i<33;i++)assert(pixels[i]==(i>=2&&i<10?0x404040:0));
 for(int group=0;group<4;group++){groups[group].enabled=true;groups[group].color=0xffffff;groups[group].brightness=255;groups[group].color_mode=NY_LIGHTING_FIXED;}
 for(int effect=0;effect<NY_LIGHTING_EFFECT_COUNT;effect++)for(int step=0;step<80;step++){
  for(int i=0;i<4;i++)groups[i].effect=effect;
  input.level=.7;input.music=true;ny_lighting_render(groups,&input,step*73,pixels);
  for(int i=0;i<33;i++)for(int shift=0;shift<24;shift+=8)assert(((pixels[i]>>shift)&255)<=255);
  for(int i=0;i<33;i++)assert((pixels[i]&0xff000000)==0);
 }
 for(int i=0;i<4;i++)groups[i].effect=NY_LIGHTING_MUSIC;
 input.music=false;input.level=1;ny_lighting_render(groups,&input,800,pixels);assert(black(pixels));
 input.music=true;input.level=0;ny_lighting_render(groups,&input,800,pixels);assert(black(pixels));
 input.level=.8;ny_lighting_render(groups,&input,800,pixels);assert(!black(pixels));
 for(int i=0;i<4;i++){groups[i].effect=NY_LIGHTING_STEADY;groups[i].brightness=100;}ny_lighting_render(groups,&input,0,pixels);for(int i=0;i<33;i++)assert(pixels[i]==0xffffff);
 ny_lighting_boot(input.iris,0,pixels);assert(black(pixels));ny_lighting_boot(input.iris,1200,pixels);assert(!black(pixels));ny_lighting_boot(input.iris,2600,other);assert(black(other));
 ny_lighting_sleep(pixels,1300,other);assert(black(other));
 assert(call(NY_PRODUCT_GUEST,"lighting.set","{\"group\":\"all\",\"enabled\":true}")==-EACCES);
 assert(call(NY_PRODUCT_OWNER,"lighting.status","{}")==0);
 g_lighting_running=g_lighting_available=true;g_lighting_phase=NY_LIGHTING_READY;
 assert(call(NY_PRODUCT_OWNER,"lighting.set","{\"group\":\"left_ear\",\"enabled\":true,\"color\":\"#ff0000\",\"brightness\":20}")==0);
 assert(g_lighting_groups[0].enabled&&g_lighting_groups[0].brightness==20&&g_lighting_groups[0].color_mode==NY_LIGHTING_FIXED);
 assert(call(NY_PRODUCT_OWNER,"lighting.set","{\"group\":\"all\",\"brightness\":101}")==-ERANGE);
 assert(call(NY_PRODUCT_OWNER,"lighting.set","{\"group\":\"all\",\"effect\":\"orbit\"}")==-EINVAL);
 assert(call(NY_PRODUCT_OWNER,"lighting.set","{\"group\":\"left_ear\",\"color\":\"#bad\"}")==-EINVAL);
 assert(call(NY_PRODUCT_OWNER,"lighting.set","{\"group\":\"all\",\"enabled\":true,\"revision\":999}")==-ESTALE);
 save_error=-EIO;assert(call(NY_PRODUCT_OWNER,"lighting.set","{\"group\":\"left_ear\",\"brightness\":5}")==-EIO);assert(g_lighting_groups[0].brightness==20);save_error=0;
 g_lighting_loaded=false;assert(ny_lighting_load()==0);assert(!g_lighting_groups[0].enabled&&g_lighting_groups[0].brightness==20);
 assert(call(NY_PRODUCT_FAMILY,"lighting.preset","{\"name\":\"aurora\"}")==0);assert(g_lighting_groups[3].effect==NY_LIGHTING_AURORA);
 g_lighting_sleep_input=true;assert(call(NY_PRODUCT_OWNER,"lighting.set","{\"group\":\"all\",\"enabled\":true}")==-EBUSY);
 assert(call(NY_PRODUCT_OWNER,"lighting.set","{\"group\":\"all\",\"enabled\":false}")==0);
 g_lighting_sleep_input=false;g_lighting_force_black=false;g_lighting_phase=NY_LIGHTING_READY;
 for(int i=0;i<33;i++)g_lighting_output[i]=0x202020;
 ny_lighting_update(now,&input,true,true,pixels);assert(g_lighting_phase==NY_LIGHTING_FADING_SLEEP&&!black(pixels));
 ny_lighting_update(now+1300,&input,true,true,pixels);assert(g_lighting_phase==NY_LIGHTING_ASLEEP&&black(pixels));
 ny_lighting_update(now+1400,&input,false,true,pixels);assert(g_lighting_phase==NY_LIGHTING_WAKING);
 g_lighting_phase=NY_LIGHTING_BOOT_WAIT;g_lighting_phase_since=now;g_lighting_force_black=true;g_lighting_stopping=false;failures=1;ticks=0;
 ny_lighting_worker(NULL);assert(calls>=3&&failures==0&&black(sent)&&!g_lighting_running);
 cJSON_Delete(saved);
 puts("LIGHTING_PASS zones brightness_cap iris_complement true_music boot_black sleep_fade permissions persistence atomic_failure retry_shutdown");
 return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='ny-lighting-') as d:
 root=Path(d);(root/'nuttx').mkdir();(root/'netutils').mkdir()
 (root/'nuttx/config.h').write_text('')
 (root/'nuttx/mutex.h').write_text('#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n#define nxmutex_lock pthread_mutex_lock\n#define nxmutex_unlock pthread_mutex_unlock\n')
 (root/'netutils/cJSON.h').write_text('#include <cJSON.h>\n')
 (root/'test.c').write_text(code)
 for name,flags in [('plain',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
  binary=root/name
  subprocess.run(['cc','-D_DEFAULT_SOURCE','-std=c11','-Wall','-Wextra','-Werror',*flags,'-I'+str(root),'-I'+str(args.core),'-I'+str(args.cjson),str(root/'test.c'),str(args.core/'ny_lighting_effects.c'),str(args.cjson/'cJSON.c'),'-pthread','-lm','-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
