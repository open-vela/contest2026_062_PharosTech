#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise production plugin upload staging and signed-install boundary."""
import argparse, subprocess, tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('core',type=Path);p.add_argument('cjson',type=Path);p.add_argument('mbedtls',type=Path);a=p.parse_args()
with tempfile.TemporaryDirectory() as directory:
 root=Path(directory);(root/'nuttx').mkdir();(root/'netutils').mkdir();(root/'nuttx/config.h').write_text('');(root/'netutils/cJSON.h').symlink_to((a.cjson/'cJSON.h').resolve())
 source=root/'test.c';source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static size_t test_strlcpy(char *d,const char*s,size_t n){size_t l=strlen(s);if(n)snprintf(d,n,"%s",s);return l;}
#define strlcpy test_strlcpy
#include "ny_plugin_upload.c"
static int installs;
static int verification;
uint64_t ny_product_time_ms(bool monotonic){return 100;}
int ny_manifest_load(const char *path,struct ny_plugin_config_s *c){memset(c,0,sizeof(*c));strcpy(c->id,"test");strcpy(c->version,"1");return verification;}
int ny_package_install(const char *path){installs++;return 0;}
int ny_package_resolve(const char *id,char *path,size_t n){return -ENOENT;}
int ny_package_activate(const char *id,const char *version){return 0;}
static int call(struct ny_product_caller_s *c,const char*t,const char*s,cJSON **r){cJSON*d=cJSON_Parse(s);*r=NULL;int x=ny_plugin_upload_request(c,t,d,r);cJSON_Delete(d);return x;}
int main(void){
 struct ny_product_caller_s c={"alice",NY_PRODUCT_OWNER,true};cJSON*r;char id[64],json[512],root[PATH_MAX];
 c.role=NY_PRODUCT_GUEST;assert(call(&c,"plugin.upload.begin","{}",&r)==-EACCES);c.role=NY_PRODUCT_OWNER;c.local_transport=false;assert(call(&c,"plugin.upload.begin","{}",&r)==-EACCES);c.local_transport=true;
 assert(call(&c,"plugin.upload.begin","{}",&r)==0);strcpy(id,ny_plugin_upload_string(r,"uploadId"));strcpy(root,g_upload.root);cJSON_Delete(r);
 assert(call(&c,"plugin.upload.begin","{}",&r)==-EBUSY);
 snprintf(json,sizeof(json),"{\"uploadId\":\"%s\",\"path\":\"../escape\",\"offset\":0,\"data\":\"eA==\"}",id);assert(call(&c,"plugin.upload.chunk",json,&r)==-EINVAL);
 snprintf(json,sizeof(json),"{\"uploadId\":\"%s\",\"path\":\"sub/main.js\",\"offset\":0,\"data\":\"eA==\"}",id);c.id="bob";assert(call(&c,"plugin.upload.chunk",json,&r)==-ENOENT);c.id="alice";assert(call(&c,"plugin.upload.chunk",json,&r)==0);cJSON_Delete(r);assert(g_upload.bytes==1);assert(call(&c,"plugin.upload.chunk",json,&r)==-EINVAL);
 snprintf(json,sizeof(json),"{\"uploadId\":\"%s\"}",id);verification=-EACCES;assert(call(&c,"plugin.upload.commit",json,&r)==-EACCES);assert(installs==0);assert(access(root,F_OK)<0);
 assert(call(&c,"plugin.upload.begin","{}",&r)==0);strcpy(id,ny_plugin_upload_string(r,"uploadId"));strcpy(root,g_upload.root);cJSON_Delete(r);
 snprintf(json,sizeof(json),"{\"uploadId\":\"%s\",\"path\":\"manifest.json\",\"offset\":0,\"data\":\"e30=\"}",id);assert(call(&c,"plugin.upload.chunk",json,&r)==0);cJSON_Delete(r);
 snprintf(json,sizeof(json),"{\"uploadId\":\"%s\"}",id);verification=0;assert(call(&c,"plugin.upload.commit",json,&r)==0);cJSON_Delete(r);assert(installs==1);assert(access(root,F_OK)<0);
 puts("NY_PLUGIN_UPLOAD_PASS");return 0;
}
''')
 for flags in [['-O2'],['-O1','-g','-fsanitize=address,undefined']]:
  binary=root/'test';subprocess.run(['cc','-std=c11','-D_GNU_SOURCE',*flags,'-I'+str(root),'-I'+str(a.core),'-I'+str(a.mbedtls/'include'),str(source),str(a.cjson/'cJSON.c'),str(a.mbedtls/'library/base64.c'),str(a.mbedtls/'library/constant_time.c'),'-ffunction-sections','-Wl,--gc-sections','-o',str(binary)],check=True);subprocess.run([str(binary)],check=True)
