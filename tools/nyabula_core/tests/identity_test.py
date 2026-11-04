#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Test identity enrollment against the real persistent-store contract."""
import sys,subprocess,tempfile
from pathlib import Path
core,cjson=map(Path,sys.argv[1:3])
baseline=Path(sys.argv[3]) if len(sys.argv)>3 else core
code=r'''#include <assert.h>
#include <stdlib.h>
#include "ny_product_identity.c"
#include "ny_product_store.c"
static char *saved;
static int save_error;
static bool camera=true,identifying=true;
static uint64_t now=1000;
bool ny_product_privacy_camera_allowed(void){return camera;}
bool ny_product_vision_identifying(void){return identifying;}
uint64_t ny_product_time_ms(bool monotonic){(void)monotonic;return now;}
int ny_state_read(const char *key,char **value,size_t *length,size_t limit){
 assert(!strcmp(key,"product/v1/identity"));*value=NULL;*length=0;if(!saved)return -ENOENT;
 *length=strlen(saved);assert(*length<limit);*value=strdup(saved);return 0;
}
int ny_state_write(const char *key,const void *value,size_t length){
 assert(!strcmp(key,"product/v1/identity"));if(save_error)return save_error;
 free(saved);saved=strndup(value,length);return 0;
}
static cJSON *out;
static int call(int role,const char *topic,const char *json){
 cJSON_Delete(out);out=NULL;cJSON *data=cJSON_Parse(json);
 struct ny_product_caller_s caller={"test",role,true};
 int ret=ny_product_identity_request(&caller,topic,data,&out);cJSON_Delete(data);return ret;
}
static void sample(const int16_t *feature,unsigned int count){now+=500;ny_product_identity_observe(feature,count,now);}
int main(void){
 assert(call(NY_PRODUCT_FAMILY,"identity.status","{}")==-EACCES);
 assert(call(NY_PRODUCT_OWNER,"identity.status","{}")==0);
 assert(cJSON_GetArraySize(cJSON_GetObjectItem(out,"members"))==0);
 camera=false;assert(call(NY_PRODUCT_OWNER,"identity.enroll.start","{\"name\":\"Owner\",\"revision\":0}")==-EACCES);camera=true;
 identifying=false;assert(call(NY_PRODUCT_OWNER,"identity.enroll.start","{\"name\":\"Owner\",\"revision\":0}")==-EACCES);identifying=true;
 assert(call(NY_PRODUCT_OWNER,"identity.enroll.start","{\"name\":\"Owner\",\"revision\":0}")==0);
 int16_t feature[128]={32767},other[128]={0,32767};
 ny_product_identity_observe(feature,1,now-1);assert(g_identity_samples==0);
 sample(feature,2);assert(g_identity_samples==0&&!strcmp(g_identity_reason,"one_face"));
 for(int i=0;i<6;i++)sample(feature,1);
 assert(!strcmp(g_identity_phase,"saved")&&g_identity_revision==1);
 assert(call(NY_PRODUCT_OWNER,"identity.status","{}")==0);
 char *text=cJSON_PrintUnformatted(out);assert(!strstr(text,"feature")&&strstr(text,"Owner"));cJSON_free(text);
 sample(feature,1);assert(call(NY_PRODUCT_OWNER,"presence.status","{}")==0);
 cJSON *member=cJSON_GetArrayItem(cJSON_GetObjectItem(out,"members"),0);assert(cJSON_IsTrue(cJSON_GetObjectItem(member,"recent")));
 now+=4000;sample(other,1);assert(call(NY_PRODUCT_OWNER,"presence.status","{}")==0);
 member=cJSON_GetArrayItem(cJSON_GetObjectItem(out,"members"),0);assert(!cJSON_IsTrue(cJSON_GetObjectItem(member,"recent")));
 assert(call(NY_PRODUCT_OWNER,"identity.enroll.start","{\"name\":\"Duplicate\",\"revision\":1}")==0);sample(feature,1);
 assert(!strcmp(g_identity_reason,"duplicate")&&g_identity_revision==1);
 assert(call(NY_PRODUCT_OWNER,"identity.enroll.start","{\"name\":\"Family\",\"revision\":1}")==0);
 for(int i=0;i<5;i++)sample(other,1);
 save_error=-ENOSPC;sample(other,1);save_error=0;
 assert(!strcmp(g_identity_reason,"storage")&&g_identity_revision==1);
 cJSON_Delete(g_identity_record);g_identity_record=NULL;
 assert(call(NY_PRODUCT_OWNER,"identity.status","{}")==0);assert(cJSON_GetArraySize(cJSON_GetObjectItem(out,"members"))==1);
 assert(call(NY_PRODUCT_OWNER,"identity.rename","{\"id\":\"person-1\",\"name\":\"Renamed\",\"revision\":0}")==-ESTALE);
 assert(call(NY_PRODUCT_OWNER,"identity.rename","{\"id\":\"person-1\",\"name\":\"Renamed\",\"revision\":1}")==0);
 assert(call(NY_PRODUCT_OWNER,"identity.remove","{\"id\":\"person-1\",\"revision\":2}")==0);
 assert(cJSON_GetArraySize(cJSON_GetObjectItem(out,"members"))==0);
 assert(call(NY_PRODUCT_OWNER,"identity.enroll.start","{\"name\":\"Expired\",\"revision\":3}")==0);
 now+=30001;assert(call(NY_PRODUCT_OWNER,"identity.status","{}")==0);assert(!strcmp(g_identity_reason,"timeout"));
 cJSON_Delete(out);cJSON_Delete(g_identity_record);free(saved);
 puts("IDENTITY_PASS real_store owner privacy explicit_enroll samples duplicate unknown presence_expiry failure_atomic rename delete timeout no_vector_disclosure");
}
'''
with tempfile.TemporaryDirectory(prefix='ny-identity-') as tmp:
 t=Path(tmp);(t/'nuttx').mkdir();(t/'netutils').mkdir()
 (t/'nuttx/config.h').write_text('#define CONFIG_NYABULA_CORE_PRODUCT 1\n')
 (t/'nuttx/mutex.h').write_text('#pragma once\n#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n#define nxmutex_lock pthread_mutex_lock\n#define nxmutex_unlock pthread_mutex_unlock\n')
 (t/'netutils/cJSON.h').write_text('#include <cJSON.h>\n');(t/'test.c').write_text(code)
 for name,flags in [('plain',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
  exe=t/name
  subprocess.run(['cc','-D_DEFAULT_SOURCE','-DFAR=','-std=c11','-Wall','-Wextra','-Werror',*flags,'-I'+str(t),'-I'+str(core),'-I'+str(baseline),'-I'+str(cjson),str(t/'test.c'),str(cjson/'cJSON.c'),'-pthread','-lm','-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
