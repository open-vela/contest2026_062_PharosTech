#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real camera-to-vision worker with an in-memory RPMsg peer."""
import sys,tempfile,subprocess
from pathlib import Path
core,cjson=map(Path,sys.argv[1:3]);proto=core.parents[1]/'tools/amp/protocol'
code=r"""
#include <assert.h>
#include <stdio.h>
int task_create(const char *,int,int,int (*)(int,char **),char *const []);
#include "ny_product_vision.c"
static unsigned char shared[4*1024*1024],packet[NYAMP_RPMSG_MTU];
static size_t packet_size;
static bool allowed=true;
static unsigned int frames,captured,followed,spoken,closed;
static int (*pending)(int,char **);
static uint64_t now=1000;
uint64_t ny_product_time_ms(bool monotonic){(void)monotonic;return now;}
bool ny_product_privacy_camera_allowed(void){return allowed;}
bool rk3576_shmem_ready(void){return true;}
int rk3576_shmem_initialize(void){return 0;}
void *rk3576_shmem_base(void){return shared;}
uint32_t ny_compute_generation(void){return 9;}
uint64_t ny_compute_request_id(void){static uint64_t id;return ++id;}
int ny_compute_port_open(struct ny_compute_port_s **port){*port=(void*)1;return 0;}
void ny_compute_port_close(struct ny_compute_port_s *port){assert(port==(void*)1);}
int ny_compute_port_send(struct ny_compute_port_s *port,const uint8_t *data,size_t size){
 (void)port;nyamp_header_s h;assert(nyamp_header_decode(&h,data,size)==0);
 uint8_t body[NYAMP_INLINE_MAX]={0};size_t bytes=0;
 const uint8_t *input=data+NYAMP_WIRE_HEADER_SIZE;
 if(h.opcode==NYAMP_VISION_TRACK_LOAD){assert(h.payload_size==strlen("vision/tracker")&&!memcmp(input,"vision/tracker",h.payload_size));}
 else if(h.opcode==NYAMP_VISION_BEGIN){nyamp_buffer_s grant={0};grant.lease=1;grant.generation=9;grant.offset=NYAMP_SLOT_VISION;grant.capacity=NYAMP_SLOT_VISION_SIZE;grant.format=NYAMP_FORMAT_BYTES;grant.flags=NYAMP_BUFFER_IN_SHMEM;assert(nyamp_buffer_encode(body,sizeof(body),&bytes,&grant)==0);}
 else if(h.opcode==NYAMP_VISION_TRACK_INIT||h.opcode==NYAMP_VISION_TRACK_FRAME){
  assert(captured>=32);frames++;
  if(frames==1)assert(h.opcode==NYAMP_VISION_TRACK_INIT&&h.payload_size==NYAMP_VISION_TRACK_INIT_SIZE);
  nyamp_vision_put(body,nyamp_vision_get(input+NYAMP_BUFFER_SIZE,8),8);
  nyamp_vision_put(body+8,nyamp_vision_get(input+NYAMP_BUFFER_SIZE+8,8),8);
  nyamp_vision_put(body+16,frames==1?1:frames==2?3:2,4);
  nyamp_vision_put(body+20,frames>=3?100:20,4);nyamp_vision_put(body+24,2,4);
  const float values[]={.25f,.25f,.5f,.5f,.95f};
  for(int i=0;i<5;i++){nyamp_vision_put_float(body+28+i*4,values[i]);}
  bytes=48;
 }
 else assert(h.opcode==NYAMP_VISION_END);
 h.flags=NYAMP_FLAG_RESPONSE;h.payload_size=bytes+4;packet_size=NYAMP_WIRE_HEADER_SIZE+h.payload_size;
 assert(nyamp_header_encode(packet,sizeof(packet),&h)==0);
 nyamp_vision_put(packet+NYAMP_WIRE_HEADER_SIZE,0,4);memcpy(packet+NYAMP_WIRE_HEADER_SIZE+4,body,bytes);return 0;
}
ssize_t ny_compute_port_recv(struct ny_compute_port_s *port,uint8_t *data,size_t size,int timeout){(void)port;(void)timeout;assert(size>=packet_size);memcpy(data,packet,packet_size);return packet_size;}
int ny_product_camera_subscribe(struct ny_camera_client_s *camera){camera->active=true;return 0;}
void ny_product_camera_close(struct ny_camera_client_s *camera){if(camera->active)closed++;camera->active=false;}
int ny_product_camera_raw_next(struct ny_camera_client_s *camera,uint8_t *data,size_t size,uint64_t *sequence,uint64_t *timestamp){assert(camera->active&&size==NYAMP_VISION_BYTES);captured++;if(captured==35){g_vision_stop=true;return -ECANCELED;}memset(data,128,size);*sequence=captured;*timestamp=now=1000+captured*100;return 0;}
int ny_product_camera_gaze_request(const struct ny_product_caller_s *caller,const char *topic,const cJSON *data,cJSON **out){assert(caller->role==NY_PRODUCT_OWNER&&!strcmp(topic,"camera.point"));assert(cJSON_GetObjectItem(data,"u")->valuedouble==.5);followed++;*out=cJSON_CreateObject();return 0;}
void ny_product_identity_observe(const int16_t *v,unsigned int faces,uint64_t stamp){(void)v;(void)faces;(void)stamp;assert(false);}
void ny_product_identity_faces(unsigned int faces,uint64_t stamp){(void)faces;(void)stamp;assert(false);}
void ny_product_identity_clear_live(void){}
int ny_voice_request(const struct ny_product_caller_s *caller,const char *topic,const cJSON *data,cJSON **out){(void)caller;assert(!strcmp(topic,"voice.say")&&cJSON_IsString(cJSON_GetObjectItem(data,"text")));spoken++;*out=cJSON_CreateObject();return 0;}
int task_create(const char *name,int priority,int stack,int (*entry)(int,char**),char *const argv[]){(void)priority;(void)stack;(void)argv;assert(!strcmp(name,"ny_vision"));pending=entry;return 100;}
static int call(int role,const char *topic,const char *json){struct ny_product_caller_s caller={"fixture",role,true};cJSON *data=cJSON_Parse(json),*out=NULL;int ret=ny_product_vision_request(&caller,topic,data,&out);cJSON_Delete(data);cJSON_Delete(out);return ret;}
int main(void){
 assert(call(NY_PRODUCT_FAMILY,"vision.learn","{}")==-EACCES);
 allowed=false;assert(call(NY_PRODUCT_OWNER,"vision.learn","{}")==-EACCES);allowed=true;
 assert(call(NY_PRODUCT_OWNER,"vision.learn","{\"width\":0}")==-EINVAL);
 assert(call(NY_PRODUCT_OWNER,"vision.learn","{\"x\":0.9}")==-EINVAL);
 assert(!pending&&captured==0);
 assert(call(NY_PRODUCT_OWNER,"vision.learn","{}")==0&&pending);
 assert(call(NY_PRODUCT_OWNER,"vision.start","{}")==-EBUSY);
 cJSON *stale=cJSON_Parse("{\"capturedMs\":0,\"phase\":\"tracking\",\"faces\":[],\"target\":{\"x\":0.25,\"y\":0.25,\"width\":0.5,\"height\":0.5}}");
 ny_vision_follow(stale);assert(followed==0);cJSON_Delete(stale);
 assert(pending(0,NULL)==0);pending=NULL;
 assert(frames==3&&followed==1&&spoken==1&&closed==1&&!g_vision_running&&g_vision_error==0);
 assert(call(NY_PRODUCT_OWNER,"vision.status","{}")==0);
 puts("VISION_PRODUCT_PASS owner privacy roi warmup stale_and_lost_no_gaze trusted_follow spoken_ready cleanup");
}
"""
# The protocol C API uses explicit struct tags.
code=code.replace('nyamp_header_s h','struct nyamp_header_s h').replace('nyamp_buffer_s grant','struct nyamp_buffer_s grant')
with tempfile.TemporaryDirectory(prefix='ny-vision-') as tmp:
 t=Path(tmp)
 for name in ['nuttx','netutils','arch/chip']:(t/name).mkdir(parents=True,exist_ok=True)
 (t/'nuttx/config.h').write_text('#define CONFIG_NYABULA_CORE_PRODUCT 1\n#define CONFIG_NYABULA_CORE_COMPUTE 1\n#define CONFIG_NYABULA_CORE_OBJECT_TRACKING 1\n#define CONFIG_NYABULA_CORE_VOICE 1\n')
 (t/'nuttx/mutex.h').write_text('#pragma once\n#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n#define nxmutex_lock pthread_mutex_lock\n#define nxmutex_unlock pthread_mutex_unlock\n')
 (t/'arch/chip/rk3576_shmem.h').write_text('#include <stdbool.h>\nbool rk3576_shmem_ready(void);\nint rk3576_shmem_initialize(void);\nvoid *rk3576_shmem_base(void);\n')
 (t/'arch/chip/rk3576_shmem_layout.h').write_text('#define NYAMP_SLOT_VISION 0x160000\n#define NYAMP_SLOT_VISION_SIZE 0x100000\n')
 (t/'netutils/cJSON.h').write_text('#include <cJSON.h>\n');(t/'test.c').write_text(code)
 for flags in [['-O2'],['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all']]:
  exe=t/'test'
  subprocess.run(['cc','-D_DEFAULT_SOURCE','-DFAR=','-std=c11','-Wall','-Wextra','-Werror',*flags,'-I'+str(t),'-I'+str(core),'-I'+str(proto),'-I'+str(cjson),str(t/'test.c'),str(cjson/'cJSON.c'),str(proto/'nyamp_protocol.c'),'-pthread','-lm','-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
