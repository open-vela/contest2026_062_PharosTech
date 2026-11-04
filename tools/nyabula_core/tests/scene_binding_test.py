#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile the real device-owned scene binding with local service fixtures."""
import argparse,subprocess,tempfile
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('core',type=Path);parser.add_argument('cjson',type=Path);args=parser.parse_args()
code=r"""
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ny_product_scene.c"
static uint64_t now=100000;
static int submissions,reads;
static bool deleted;
static bool peripherals;
static int remaining=0;
static cJSON *last;
uint64_t ny_product_time_ms(bool monotonic){(void)monotonic;return now;}
int nyabula_eye_service_submit(const char *source,const char *json,size_t length){
 assert(!strcmp(source,"webui")&&strlen(json)==length);cJSON_Delete(last);last=cJSON_Parse(json);assert(last);submissions++;return 0;
}
int ny_product_request(const struct ny_product_caller_s *caller,const char *topic,const cJSON *request,cJSON **out){
 (void)request;reads++;assert(!strcmp(caller->id,"owner"));
 if(!strcmp(topic,"timer.list")){
  char text[700];snprintf(text,sizeof(text),"{\"items\":[{\"id\":\"a\",\"kind\":\"countdown\",\"status\":\"running\",\"duration_ms\":10000,\"remaining_ms\":9000,\"elapsed_ms\":1000,\"label\":\"Other\"}%s]}",deleted?"":",{\"id\":\"b\",\"kind\":\"countdown\",\"status\":\"paused\",\"duration_ms\":10000,\"remaining_ms\":0,\"elapsed_ms\":10000,\"label\":\"Selected\"}");
  *out=cJSON_Parse(text);if(!deleted)cJSON_ReplaceItemInObject(cJSON_GetArrayItem(cJSON_GetObjectItem(*out,"items"),1),"remaining_ms",cJSON_CreateNumber(remaining));return 0;
 }
 if(!strcmp(topic,"alarm.list")){*out=cJSON_Parse("{\"items\":[{\"id\":\"midnight\",\"time\":\"00:00\",\"next_at\":1000000,\"status\":\"scheduled\",\"label\":\"Midnight\"}]}");return 0;}
 if(!strcmp(topic,"calendar.list")){*out=cJSON_Parse("{\"items\":[{\"id\":\"event\",\"start_at\":0,\"title\":\"Zero hour\"}]}");return 0;}
 if(!strcmp(topic,"task.list")){*out=cJSON_Parse("{\"items\":[{\"id\":\"task\",\"state\":\"running\",\"title\":\"Quarter\",\"progress\":25}]}");return 0;}
 if(!strcmp(topic,"device.status")){
  if(caller->role!=NY_PRODUCT_OWNER)return -EACCES;
  *out=cJSON_Parse("{\"cpu\":{\"available\":true,\"percent\":0},\"memory\":{\"totalBytes\":100,\"usedBytes\":0},\"uptimeMs\":60000,\"boardModel\":\"K7\",\"network\":{\"interfaces\":[]},\"audioDevices\":[],\"storage\":[]}");if(peripherals)cJSON_AddItemToObject(*out,"peripherals",cJSON_Parse("[{\"kind\":\"camera\",\"available\":true},{\"kind\":\"lighting\",\"available\":true}]"));return 0;
 }
 if(!strcmp(topic,"dsp.status")){*out=cJSON_Parse("{\"running\":false,\"config\":{\"eqEnabled\":true,\"bypass\":false,\"masterDb\":-18,\"eqBands16\":[{\"frequencyHz\":1000,\"q\":1,\"gainDb\":6,\"enabled\":true,\"type\":\"peak\"}]}}");return 0;}
 return -ENOSYS;
}
static int show(const struct ny_product_caller_s *caller,const char *text){cJSON *input=cJSON_Parse(text),*output=NULL;int ret=ny_product_scene_request(caller,"scene.show",input,&output);cJSON_Delete(input);cJSON_Delete(output);return ret;}
static const cJSON *payload(void){return cJSON_GetObjectItem(cJSON_GetObjectItem(last,"params"),"payload");}
int main(void){
 struct ny_product_caller_s owner={"owner",NY_PRODUCT_OWNER,true},guest={"owner",NY_PRODUCT_GUEST,true},family={"owner",NY_PRODUCT_FAMILY,true};
 assert(show(&guest,"{\"scene\":\"timer\"}")==-EACCES);
 assert(show(&owner,"{\"scene\":\"timer\",\"recordId\":\"b\"}")==0);
 assert(cJSON_IsTrue(cJSON_GetObjectItem(payload(),"live_data")));
 assert(ny_product_scene_number(payload(),"remaining_ms")==0);
 assert(!strcmp(ny_product_scene_text(payload(),"title"),"Selected"));
 assert(show(&owner,"{\"scene\":\"timer\",\"style\":\"minimal\"}")==0);
 assert(!strcmp(g_scene_binding.record,"b"));
 remaining=8000;now+=1001;assert(ny_product_scene_tick()==0);assert(ny_product_scene_number(payload(),"remaining_ms")==8000);
 int count=submissions;now+=1001;assert(ny_product_scene_tick()==0&&submissions==count);
 deleted=true;now+=1001;assert(ny_product_scene_tick()==-ENODATA);assert(!strcmp(ny_product_scene_text(last,"action"),"eyes.scene.hide"));
 deleted=false;now+=1001;assert(ny_product_scene_tick()==0&&g_scene_shown);
 assert(show(&owner,"{\"scene\":\"battery\"}")==-ENOTSUP);assert(g_scene_active&&!strcmp(g_scene_binding.type,"timer"));
 assert(ny_product_scene_manual_submit("webui","{}",2)==0);count=submissions;now+=1001;ny_product_scene_tick();assert(submissions==count&&!g_scene_active);
 assert(show(&family,"{\"scene\":\"system\"}")==-EACCES);
 assert(show(&owner,"{\"scene\":\"alarm\",\"recordId\":\"midnight\"}")==0);assert(ny_product_scene_number(payload(),"hour")==0&&ny_product_scene_number(payload(),"minute")==0);
 assert(show(&owner,"{\"scene\":\"calendar\",\"recordId\":\"event\",\"utcOffsetMinutes\":0}")==0);assert(ny_product_scene_number(payload(),"hour")==0);
 assert(show(&owner,"{\"scene\":\"task\",\"recordId\":\"task\"}")==0);assert(ny_product_scene_number(payload(),"progress")==.25&&!strcmp(ny_product_scene_text(payload(),"value"),"25%"));
 assert(show(&owner,"{\"scene\":\"devices\"}")==0);assert(ny_product_scene_number(payload(),"device_count")==0);
 peripherals=true;assert(show(&owner,"{\"scene\":\"devices\"}")==0);assert(ny_product_scene_number(payload(),"device_count")==2);
 assert(show(&owner,"{\"scene\":\"system\"}")==0);assert(!strcmp(ny_product_scene_text(payload(),"title"),"CPU 0%"));
 assert(show(&owner,"{\"scene\":\"eq\"}")==0);assert(cJSON_GetArraySize(cJSON_GetObjectItem(payload(),"eq_bands"))==10);
 cJSON *bands=cJSON_Parse("[{\"frequencyHz\":1000,\"q\":1,\"gainDb\":6,\"enabled\":true,\"type\":\"peak\"}]");assert(fabs(ny_product_scene_eq(bands,1000)-6)<.00001);cJSON_Delete(bands);
 cJSON *empty=cJSON_CreateObject(),*catalog=NULL;assert(ny_product_scene_request(&owner,"scene.list",empty,&catalog)==0);assert(cJSON_GetArraySize(cJSON_GetObjectItem(catalog,"items"))==26);cJSON_Delete(empty);cJSON_Delete(catalog);
 ny_product_scene_shutdown();assert(!g_scene_active&&!g_scene_shown);cJSON_Delete(last);
 puts("SCENE_BINDING_PASS selected_record offline_tick zero_values permissions manual_takeover empty_source eq_response");return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='ny-scene-') as d:
 root=Path(d);(root/'nuttx').mkdir();(root/'netutils').mkdir()
 (root/'nuttx/config.h').write_text('#define CONFIG_NYABULA_CORE_EYE 1\n')
 (root/'nuttx/mutex.h').write_text('#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n#define nxmutex_lock pthread_mutex_lock\n#define nxmutex_unlock pthread_mutex_unlock\n')
 (root/'netutils/cJSON.h').write_text('#include <cJSON.h>\n')
 (root/'nyabula_eye_service.h').write_text('#include <stddef.h>\nint nyabula_eye_service_submit(const char *,const char *,size_t);\n')
 (root/'test.c').write_text(code)
 for name,flags in [('plain',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
  binary=root/name
  subprocess.run(['cc','-D_DEFAULT_SOURCE','-std=c11','-Wall','-Wextra','-Werror',*flags,'-I'+str(root),'-I'+str(args.core),'-I'+str(args.cjson),str(root/'test.c'),str(args.cjson/'cJSON.c'),'-pthread','-lm','-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
