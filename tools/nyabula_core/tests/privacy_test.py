#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise persisted privacy policy, permissions, CAS and producer shutdown."""
import sys, subprocess, tempfile
from pathlib import Path
core,cjson=map(Path,sys.argv[1:])
code=r'''#include <assert.h>
#include <stdio.h>
#include "ny_product_privacy.c"
#include "ny_product_store.c"
static char *saved;
static int failure, read_failure, camera_stops, mic_stops, voice_stops, voice_starts;
int ny_state_read(const char *key,char **out,size_t *length,size_t limit) {
 assert(!strcmp(key,"product/v1/privacy"));*out=NULL;*length=0;
 if(read_failure)return read_failure;
 if(!saved)return -ENOENT;
 *length=strlen(saved);assert(*length<limit);*out=strdup(saved);return 0;
}
int ny_state_write(const char *key,const void *value,size_t length) {
 assert(!strcmp(key,"product/v1/privacy"));if(failure)return failure;
 free(saved);saved=strndup(value,length);return 0;
}
void ny_product_camera_shutdown(void){assert(!ny_product_privacy_camera_allowed());camera_stops++;}
void ny_product_intercom_shutdown(void){assert(!ny_product_privacy_mic_allowed());mic_stops++;}
int ny_voice_stop(void){assert(!ny_product_privacy_mic_allowed());voice_stops++;return 0;}
int ny_voice_start(void){assert(ny_product_privacy_mic_allowed());voice_starts++;return 0;}
static int request(int role,const char *topic,const char *json) {
 struct ny_product_caller_s caller={"fixture",role,true};cJSON *data=cJSON_Parse(json),*out=NULL;
 int ret=ny_product_privacy_request(&caller,topic,data,&out);cJSON_Delete(data);cJSON_Delete(out);return ret;
}
int main(void) {
 assert(ny_product_privacy_camera_allowed()&&ny_product_privacy_mic_allowed());
 assert(request(NY_PRODUCT_FAMILY,"privacy.set","{\"camera\":false,\"revision\":0}")==-EACCES);
 assert(request(NY_PRODUCT_OWNER,"privacy.set","{\"camera\":false,\"revision\":0}")==0);
 assert(!ny_product_privacy_camera_allowed()&&ny_product_privacy_mic_allowed()&&camera_stops==1);
 assert(request(NY_PRODUCT_OWNER,"privacy.set","{\"mic\":false,\"revision\":0}")==-ESTALE);
 failure=-ENOSPC;
 assert(request(NY_PRODUCT_OWNER,"privacy.set","{\"mic\":false,\"revision\":1}")==-ENOSPC);
 assert(ny_product_privacy_mic_allowed()&&mic_stops==0&&g_privacy_revision==1);failure=0;
 assert(request(NY_PRODUCT_OWNER,"privacy.set","{\"mic\":false,\"revision\":1}")==0);
 assert(!ny_product_privacy_mic_allowed()&&mic_stops==1&&voice_stops==1);
 g_privacy_loaded=false;assert(!ny_product_privacy_camera_allowed()&&!ny_product_privacy_mic_allowed());
 assert(request(NY_PRODUCT_OWNER,"privacy.set","{\"mic\":true,\"revision\":2}")==0&&voice_starts==1);
 assert(!ny_product_privacy_camera_allowed()&&ny_product_privacy_mic_allowed());
 assert(request(NY_PRODUCT_OWNER,"privacy.set","{\"camera\":1,\"revision\":3}")==-EINVAL);
 assert(request(NY_PRODUCT_OWNER,"privacy.set","{\"camera\":true,\"revision\":3.5}")==-EINVAL);
 g_privacy_loaded=false;read_failure=-EIO;assert(!ny_product_privacy_camera_allowed()&&!ny_product_privacy_mic_allowed());
 read_failure=0;assert(ny_product_privacy_mic_allowed());free(saved);
 puts("PRIVACY_PASS real_store_empty_record permissions cas persistence store_failure gates_before_shutdown independent_controls");
}
'''
with tempfile.TemporaryDirectory(prefix='ny-privacy-') as tmp:
 t=Path(tmp);(t/'nuttx').mkdir();(t/'netutils').mkdir()
 (t/'nuttx/config.h').write_text('#define CONFIG_NYABULA_CORE_PRODUCT 1\n#define CONFIG_NYABULA_CORE_CAMERA 1\n#define CONFIG_NYABULA_CORE_INTERCOM 1\n#define CONFIG_NYABULA_CORE_VOICE 1\n')
 (t/'nuttx/mutex.h').write_text('#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n#define nxmutex_lock pthread_mutex_lock\n#define nxmutex_unlock pthread_mutex_unlock\n')
 (t/'netutils/cJSON.h').write_text('#include <cJSON.h>\n');(t/'test.c').write_text(code)
 for name,flags in [('plain',['-O2']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
  exe=t/name
  subprocess.run(['cc','-D_DEFAULT_SOURCE','-DFAR=','-std=c11','-Wall','-Wextra','-Werror',*flags,'-I'+str(t),'-I'+str(core),'-I'+str(cjson),str(t/'test.c'),str(cjson/'cJSON.c'),'-pthread','-lm','-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
