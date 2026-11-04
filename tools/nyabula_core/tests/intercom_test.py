#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real session-owned intercom with a synthetic PCM device."""
import argparse
import subprocess
import tempfile
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('core', type=Path)
p.add_argument('cjson', type=Path)
p.add_argument('mbedtls', type=Path)
a = p.parse_args()
code = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include "ny_product_intercom.c"
struct ny_audio_stream_s { bool capture; };
static atomic_int claims, opened, fail_open, fail_claim, short_write;
static atomic_uint_fast64_t offset_ms;
uint64_t ny_product_time_ms(bool mono){(void)mono;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000+atomic_load(&offset_ms);}
int ny_voice_speaker_claim(void){atomic_fetch_add(&claims,1);return atomic_load(&fail_claim);}
void ny_voice_speaker_release(void){assert(atomic_fetch_sub(&claims,1)==1);}
int ny_audio_stream_open(struct ny_audio_stream_s **stream,const struct ny_audio_stream_config_s *config){
 assert(atomic_load(&claims)==1);assert(config->rate==16000&&config->channels==1&&config->dsp_source==NULL);
 if(atomic_load(&fail_open))return atomic_load(&fail_open);
 assert(atomic_fetch_add(&opened,1)==0);*stream=malloc(sizeof(**stream));assert(*stream);(*stream)->capture=config->capture;return 0;
}
void ny_audio_stream_close(struct ny_audio_stream_s *stream){if(stream){assert(atomic_fetch_sub(&opened,1)==1);free(stream);}}
ssize_t ny_audio_stream_read(struct ny_audio_stream_s *stream,void *pcm,size_t size,int timeout){
 assert(stream->capture&&timeout==250&&size==3200);usleep(2000);unsigned char *p=pcm;for(size_t i=0;i<size;i+=2){p[i]=0xe8;p[i+1]=3;}return size;
}
ssize_t ny_audio_stream_write(struct ny_audio_stream_s *stream,const void *pcm,size_t size,int timeout){
 assert(!stream->capture&&timeout==250&&size<=3200);assert(((const unsigned char*)pcm)[0]==0xe8);return atomic_load(&short_write)?(ssize_t)size-2:(ssize_t)size;
}
int ny_audio_stream_drain(struct ny_audio_stream_s *stream,int timeout){assert(!stream->capture&&timeout==500);return 0;}
static int call(struct ny_intercom_client_s *client,const char *topic,const char *json,enum ny_product_role_e role){
 struct ny_product_caller_s caller={"test",role,true};cJSON *in=cJSON_Parse(json),*out=NULL;assert(in);int ret=ny_product_intercom_request(&caller,client,topic,in,&out);cJSON_Delete(in);cJSON_Delete(out);return ret;
}
static void wait_phase(const char *wanted){
 for(int i=0;i<2000;i++){nxmutex_lock(&g_intercom_lock);bool done=!strcmp(g_intercom_phase,wanted);nxmutex_unlock(&g_intercom_lock);if(done)return;usleep(1000);}assert(!"intercom phase timed out");
}
static char *audio_json(uint32_t sequence){
 unsigned char bytes[3200],encoded[4269];size_t written;for(size_t i=0;i<sizeof(bytes);i+=2){bytes[i]=0xe8;bytes[i+1]=3;}
 assert(mbedtls_base64_encode(encoded,sizeof(encoded),&written,bytes,sizeof(bytes))==0);
 cJSON *data=cJSON_CreateObject();cJSON_AddNumberToObject(data,"sequence",sequence);cJSON_AddStringToObject(data,"data",(char*)encoded);char *out=cJSON_PrintUnformatted(data);cJSON_Delete(data);return out;
}
int main(void){
 struct ny_intercom_client_s owner={0},other={0};const char *topic=NULL;cJSON *out=NULL;
 assert(call(&owner,"intercom.start","{}",NY_PRODUCT_GUEST)==-EACCES);assert(!g_intercom_running);
 assert(call(&owner,"intercom.start","{}",NY_PRODUCT_OWNER)==0);wait_phase("listening");
 assert(call(&other,"intercom.start","{}",NY_PRODUCT_OWNER)==-EBUSY);
 assert(call(&other,"intercom.stop","{}",NY_PRODUCT_OWNER)==0);assert(!g_intercom_stop);
 bool audio=false;for(int i=0;i<100;i++){
  int ret=ny_product_intercom_next(&owner,&topic,&out);
  if(ret==0){if(!strcmp(topic,"intercom.audio")){assert(cJSON_GetObjectItem(out,"samples")->valueint==1600);audio=true;}cJSON_Delete(out);out=NULL;if(audio)break;}
  else assert(ret==-EAGAIN);
  usleep(1000);
 }
 assert(audio&&atomic_load(&claims)==1&&atomic_load(&opened)==1);
 char *packet=audio_json(0);
 assert(call(&owner,"intercom.audio",packet,NY_PRODUCT_OWNER)==-EACCES);
 assert(call(&owner,"intercom.talk.start","{}",NY_PRODUCT_OWNER)==0);wait_phase("talking");
 assert(call(&other,"intercom.audio",packet,NY_PRODUCT_OWNER)==-ENODEV);
 assert(call(&owner,"intercom.audio","{\"sequence\":0,\"data\":\"AA==\"}",NY_PRODUCT_OWNER)==-EINVAL);
 assert(call(&owner,"intercom.audio",packet,NY_PRODUCT_OWNER)==0);
 assert(call(&owner,"intercom.audio",packet,NY_PRODUCT_OWNER)==-ESTALE);
 nxmutex_lock(&g_intercom_lock);g_intercom_count=NY_INTERCOM_PLAY;
 cJSON *full=cJSON_Parse(packet);cJSON_ReplaceItemInObject(full,"sequence",cJSON_CreateNumber(1));assert(ny_intercom_push(full)==-EBUSY);cJSON_Delete(full);g_intercom_count=0;nxmutex_unlock(&g_intercom_lock);
 atomic_fetch_add(&offset_ms,1100);wait_phase("listening");
 assert(call(&owner,"intercom.talk.stop","{}",NY_PRODUCT_OWNER)==0);
 ny_product_intercom_close(&owner);wait_phase("off");ny_product_intercom_shutdown();
 assert(atomic_load(&claims)==0&&atomic_load(&opened)==0&&owner.id==0);
 for(size_t i=0;i<sizeof(g_intercom_capture);i++)assert(((unsigned char*)g_intercom_capture)[i]==0);
 atomic_store(&fail_open,-EBUSY);assert(call(&owner,"intercom.start","{}",NY_PRODUCT_OWNER)==0);wait_phase("error");ny_product_intercom_shutdown();assert(atomic_load(&claims)==0&&atomic_load(&opened)==0);
 atomic_store(&fail_open,0);atomic_store(&fail_claim,-ETIMEDOUT);assert(call(&owner,"intercom.start","{}",NY_PRODUCT_OWNER)==0);wait_phase("error");ny_product_intercom_shutdown();assert(atomic_load(&claims)==0);
 atomic_store(&fail_claim,0);assert(call(&owner,"intercom.start","{}",NY_PRODUCT_OWNER)==0);wait_phase("listening");assert(call(&owner,"intercom.talk.start","{}",NY_PRODUCT_OWNER)==0);wait_phase("talking");atomic_store(&short_write,1);assert(call(&owner,"intercom.audio",packet,NY_PRODUCT_OWNER)==0);wait_phase("error");ny_product_intercom_shutdown();assert(atomic_load(&claims)==0&&atomic_load(&opened)==0);
 free(packet);puts("INTERCOM_PASS owner_session bounded_pcm sequences listen_talk_switch idle_release close busy_failure claim_timeout short_write privacy_clear");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ny-intercom-') as tmp:
    t = Path(tmp)
    (t/'nuttx').mkdir(); (t/'netutils').mkdir()
    (t/'nuttx/config.h').write_text('#define CONFIG_NYABULA_CORE_VOICE 1\n#define CONFIG_NYABULA_CORE_VOICE_PRIORITY 100\n#define CONFIG_NYABULA_CORE_AUDIO_INPUT_DEVICE "/fixture/input"\n#define CONFIG_NYABULA_CORE_AUDIO_OUTPUT_DEVICE "/fixture/output"\n')
    (t/'nuttx/mutex.h').write_text('#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n#define nxmutex_lock pthread_mutex_lock\n#define nxmutex_unlock pthread_mutex_unlock\n')
    (t/'netutils/cJSON.h').write_text('#include <cJSON.h>\n')
    (t/'mbed-config.h').write_text('#define MBEDTLS_BASE64_C\n')
    (t/'test.c').write_text(code)
    for name, flags in [('plain', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'])]:
        binary = t/name
        subprocess.run(['cc', '-D_DEFAULT_SOURCE', '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                        '-DMBEDTLS_CONFIG_FILE="mbed-config.h"', '-I'+str(t), '-I'+str(a.core),
                        '-I'+str(a.cjson), '-I'+str(a.mbedtls/'include'), str(t/'test.c'),
                        str(a.cjson/'cJSON.c'), str(a.mbedtls/'library/base64.c'),
                        str(a.mbedtls/'library/constant_time.c'), '-pthread', '-lm', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
