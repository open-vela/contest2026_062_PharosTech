#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import sys,tempfile,subprocess
from pathlib import Path
core,cjson,mbed=map(Path,sys.argv[1:4]);baseline=Path(sys.argv[4]) if len(sys.argv)>4 else core
code=r'''#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
int task_create(const char *,int,int,int (*)(int,char **),char *const []);
#include "ny_product_speaker.c"
#include "ny_product_store.c"
static char *saved;
static int (*pending)(int,char**);
static bool mic=true,probe_active,cancel_verify;
static uint32_t probe_id=7,takes;
static int delivered,forgot;
static float score=.8f;
static uint64_t now=1000;
uint64_t ny_product_time_ms(bool monotonic){(void)monotonic;return now;}
bool ny_product_privacy_mic_allowed(void){return mic;}
uint32_t ny_compute_generation(void){return 9;}
uint64_t ny_compute_request_id(void){return 99;}
int ny_compute_port_open(struct ny_compute_port_s **port){*port=(void*)1;return 0;}
void ny_compute_port_close(struct ny_compute_port_s *port){assert(port==(void*)1);}
int ny_compute_port_send(struct ny_compute_port_s *port,const uint8_t *data,size_t size){(void)port;(void)data;(void)size;return 0;}
ssize_t ny_compute_port_recv(struct ny_compute_port_s *port,uint8_t *data,size_t size,int timeout){(void)port;(void)data;(void)size;(void)timeout;return 0;}
uint64_t ny_voice_capture_position(void){return 1000;}
void ny_voice_accept_speaker_wake(const struct nyamp_kws_detected_s *wake){assert(wake->label_length==5);delivered++;}
int ny_voice_request(const struct ny_product_caller_s *caller,const char *topic,const cJSON *data,cJSON **out){
 assert(caller->role==NY_PRODUCT_OWNER);*out=cJSON_CreateObject();
 if(!strcmp(topic,"voice.test.start")){probe_active=true;cJSON *test=cJSON_AddObjectToObject(*out,"test");cJSON_AddNumberToObject(test,"id",probe_id);}
 else{assert(!strcmp(topic,"voice.test.stop"));if(cJSON_GetObjectItem(data,"id")->valueint!=(int)probe_id)return -ESTALE;probe_active=false;}return 0;
}
void ny_voice_wire_init(struct ny_voice_wire_s *wire,const struct ny_voice_wire_io_s *io){memset(wire,0,sizeof(*wire));wire->io=*io;}
int ny_voice_wire_request(struct ny_voice_wire_s *wire,uint16_t service,uint16_t opcode,const uint8_t *body,size_t size,int timeout,uint8_t *reply,size_t *reply_size){
 (void)timeout;assert(service==NYAMP_SERVICE_SPEAKER);if(wire->io.interrupted(wire->io.arg))return -ECANCELED;
 if(opcode==NYAMP_SPK_LOAD)return 0;
 if(opcode==NYAMP_SPK_ENROL_BEGIN){takes=0;return 0;}
 if(opcode==NYAMP_SPK_ENROL_ADD){assert(size==24&&nyamp_spk_get(body,8)==55);nyamp_spk_put(reply,++takes,4);*reply_size=4;return 0;}
 if(opcode==NYAMP_SPK_ENROL_COMMIT){memset(reply,0,408);nyamp_spk_put(reply,4,4);nyamp_spk_float_put(reply+4,.8f);uint8_t *p=reply+8;nyamp_spk_put(p,0x5056594eU,4);nyamp_spk_put(p+4,1,2);nyamp_spk_put(p+6,192,2);nyamp_spk_put(p+8,123,4);nyamp_spk_put(p+12,4,2);nyamp_spk_put(p+16,0x3c00,2);*reply_size=408;return 0;}
 if(opcode==NYAMP_SPK_TEMPLATE_PUT){assert(size==400&&ny_speaker_template_valid(body,size));return 0;}
 if(opcode==NYAMP_SPK_VERIFY){if(cancel_verify){g_speaker_epoch++;g_speaker_stop=true;}nyamp_spk_float_put(reply,score);nyamp_spk_put(reply+4,16000,4);*reply_size=8;return 0;}
 if(opcode==NYAMP_SPK_DELETE){forgot++;return 0;}
 return -EINVAL;
}
int task_create(const char *name,int priority,int stack,int (*entry)(int,char**),char *const argv[]){(void)argv;assert(!strcmp(name,"ny_speaker")&&priority==100&&stack==16384&&!pending);pending=entry;return 100;}
int ny_state_read(const char *key,char **value,size_t *length,size_t limit){assert(!strcmp(key,"product/v1/speaker"));*value=NULL;*length=0;if(!saved)return -ENOENT;*length=strlen(saved);assert(*length<limit);*value=strdup(saved);return 0;}
int ny_state_write(const char *key,const void *value,size_t length){assert(!strcmp(key,"product/v1/speaker"));free(saved);saved=strndup(value,length);return 0;}
static cJSON *out;
static int call(int role,const char *topic,const char *json){cJSON_Delete(out);out=NULL;cJSON *data=cJSON_Parse(json);struct ny_product_caller_s caller={"fixture",role,true};int ret=ny_product_speaker_request(&caller,topic,data,&out);cJSON_Delete(data);return ret;}
static void run(void){assert(pending);int (*entry)(int,char**)=pending;pending=NULL;assert(entry(0,NULL)==0);}
int main(void){
 assert(call(NY_PRODUCT_FAMILY,"speaker.status","{}")==-EACCES);
 assert(call(NY_PRODUCT_OWNER,"speaker.status","{}")==0&&!g_speaker_enabled&&!g_speaker_template_size);
 assert(call(NY_PRODUCT_OWNER,"speaker.configure","{\"enabled\":true,\"revision\":0}")==-ENOENT);
 mic=false;assert(call(NY_PRODUCT_OWNER,"speaker.enroll.start","{}")==-EACCES);
 assert(call(NY_PRODUCT_OWNER,"speaker.prepare","{}")==0);run();assert(g_speaker_ready);mic=true;
 assert(call(NY_PRODUCT_OWNER,"speaker.enroll.start","{}")==0);run();assert(probe_active&&g_speaker_enrolling);
 struct nyamp_kws_detected_s wake={.flags=NYAMP_KWS_DETECTED_HAS_OFFSETS,.start_sample=500,.end_sample=16500,.label="hello",.label_length=5};
 assert(ny_product_speaker_on_wake(55,&wake,7)&&!pending);
 for(int i=0;i<4;i++){wake.start_sample=2000+i*20000;wake.end_sample=wake.start_sample+16000;assert(ny_product_speaker_on_wake(55,&wake,7));run();}
 assert(g_speaker_template_size==400&&!g_speaker_enabled&&!probe_active&&g_speaker_revision==1&&g_speaker_kept==4);
 assert(call(NY_PRODUCT_OWNER,"speaker.status","{}")==0);char *text=cJSON_PrintUnformatted(out);assert(!strstr(text,"template"));free(text);
 assert(!ny_product_speaker_on_wake(55,&wake,0));
 assert(call(NY_PRODUCT_OWNER,"speaker.configure","{\"enabled\":true,\"revision\":1}")==0);
 assert(ny_product_speaker_on_wake(55,&wake,0));run();assert(delivered==1&&g_speaker_matched);
 score=.2f;assert(ny_product_speaker_on_wake(55,&wake,0));run();assert(delivered==1&&!g_speaker_matched);
 assert(!ny_product_speaker_on_wake(55,&wake,42)&&!pending);
 score=.8f;cancel_verify=true;assert(ny_product_speaker_on_wake(55,&wake,0));run();assert(delivered==1);cancel_verify=false;
 mic=false;assert(call(NY_PRODUCT_OWNER,"speaker.delete","{\"revision\":2}")==0);run();assert(!g_speaker_template_size&&!g_speaker_enabled&&forgot==1);
 g_speaker_loaded=false;assert(ny_product_speaker_initialize()==0&&!g_speaker_template_size&&!g_speaker_enabled);
 cJSON_Delete(out);free(saved);
 puts("SPEAKER_PRODUCT_PASS opt_in four_distinct_wakes fresh_start private_template owner_threshold foreign_probe cancel delete_mic_off persistence");
}
'''
with tempfile.TemporaryDirectory(prefix='ny-speaker-') as tmp:
 t=Path(tmp);(t/'nuttx').mkdir();(t/'netutils').mkdir()
 (t/'nuttx/config.h').write_text('#define CONFIG_NYABULA_CORE_PRODUCT 1\n#define CONFIG_NYABULA_CORE_COMPUTE 1\n#define CONFIG_NYABULA_CORE_VOICE 1\n')
 (t/'nuttx/mutex.h').write_text('#pragma once\n#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n#define nxmutex_lock pthread_mutex_lock\n#define nxmutex_unlock pthread_mutex_unlock\n')
 (t/'netutils/cJSON.h').write_text('#include <cJSON.h>\n');(t/'mbed-config.h').write_text('#define MBEDTLS_BASE64_C\n');(t/'test.c').write_text(code)
 for name,flags in [('plain',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
  exe=t/name
  subprocess.run(['cc','-D_DEFAULT_SOURCE','-DFAR=','-DMBEDTLS_CONFIG_FILE="mbed-config.h"','-std=c11','-Wall','-Wextra','-Werror',*flags,'-I'+str(t),'-I'+str(core),'-I'+str(baseline),'-I'+str(cjson),'-I'+str(core.parents[1]/'tools/amp/protocol'),'-I'+str(mbed/'include'),str(t/'test.c'),str(cjson/'cJSON.c'),str(mbed/'library/base64.c'),str(mbed/'library/constant_time.c'),'-pthread','-lm','-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
