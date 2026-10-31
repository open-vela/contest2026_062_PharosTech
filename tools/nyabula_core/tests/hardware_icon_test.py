#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate native icon metadata and the hardware capability boundary."""
import argparse
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('core', type=Path)
parser.add_argument('cjson', type=Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='ny-hardware-') as directory:
    root = Path(directory)
    (root/'nuttx').mkdir()
    (root/'netutils').mkdir()
    (root/'nuttx/config.h').write_text('''
#define CONFIG_NYABULA_CORE_PRODUCT 1
#define CONFIG_NYABULA_CORE_MANIFEST_LIMIT 16384
#define CONFIG_NYABULA_CORE_PLUGIN_MEMORY 8388608
#define CONFIG_NYABULA_CORE_PLUGIN_STACK 262144
#define CONFIG_NYABULA_CORE_EVENT_TIMEOUT_MS 100
#define CONFIG_NYABULA_CORE_MAX_EVENTS_PER_MINUTE 600
#define CONFIG_NYABULA_CORE_EVENTS_PER_MINUTE 60
''')
    (root/'netutils/cJSON.h').symlink_to((args.cjson/'cJSON.h').resolve())
    source = root/'test.c'
    source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static size_t test_strlcpy(char *dst,const char *src,size_t size)
{size_t length=strlen(src);if(size)snprintf(dst,size,"%s",src);return length;}
#define strlcpy test_strlcpy
#include "ny_manifest.c"
#include "ny_hardware.c"
int ny_permission_apply(struct ny_plugin_config_s *config){config->permissions=0;return 0;}
int ny_revocation_check_package(const char *id,const char *version){return 0;}
static unsigned calls;
int ny_product_request(const struct ny_product_caller_s *caller,const char *topic,const cJSON *data,cJSON **result)
{assert(caller->role==NY_PRODUCT_OWNER);assert(!strcmp(caller->id,"test-plugin"));calls++;*result=cJSON_Parse("{\"ok\":true,\"hfp\":{\"number\":\"private\"},\"pairing\":{\"passkey\":\"123456\"}}");return 0;}
int ny_product_device_request(const struct ny_product_caller_s *caller,const char *topic,const cJSON *data,cJSON **result)
{calls++;*result=cJSON_Parse("{\"cpu\":\"not exposed\",\"audioDevices\":[{\"path\":\"/dev/audio/pcm0\"},{\"path\":\"/dev/audio/usbp0\"}]}");return 0;}
static int manifest(const char *directory,const char *icon,struct ny_plugin_config_s *config)
{char path[4096];snprintf(path,sizeof(path),"%s/manifest.json",directory);FILE *f=fopen(path,"w");assert(f);fprintf(f,"{\"id\":\"test-plugin\",\"name\":\"硬件助手\",\"version\":\"1\",\"apiVersion\":1,\"runtime\":\"quickjs\",\"entry\":\"main.js\",\"permissions\":[\"audio.read\",\"usb.access\",\"camera.capture\"]%s}",icon);fclose(f);return ny_manifest_load(directory,config);}
int main(int argc,char **argv)
{
 struct ny_plugin_config_s config;
 assert(manifest(argv[1],"",&config)==0);assert(!strcmp(config.icon,"extension"));assert(config.permissions==0);
 assert(manifest(argv[1],",\"icon\":\"pets\"",&config)==0);assert(!strcmp(config.icon,"pets"));
 assert(manifest(argv[1],",\"icon\":{\"type\":\"image\",\"path\":\"assets/icon.png\"}",&config)==0);
 assert(manifest(argv[1],",\"icon\":{\"type\":\"iconfont\",\"path\":\"assets/icons.woff2\",\"codepoint\":\"e601\"}",&config)==0);
 const char *bad[]={
 ",\"icon\":null",",\"icon\":\"\"",",\"icon\":\"https://bad\"",
 ",\"icon\":\"pets\",\"icon\":\"pets\"",
 ",\"icon\":{\"type\":\"image\",\"path\":\"assets/../private\"}",
 ",\"icon\":{\"type\":\"image\",\"path\":\"assets//x\"}",
 ",\"icon\":{\"type\":\"image\",\"path\":\"assets/x/\"}",
 ",\"icon\":{\"type\":\"image\",\"path\":\"data/x\"}",
 ",\"icon\":{\"type\":\"iconfont\",\"path\":\"assets/a\",\"codepoint\":\"d800\"}",
 ",\"icon\":{\"type\":\"iconfont\",\"path\":\"assets/a\",\"codepoint\":\"110000\"}"};
 for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(manifest(argv[1],bad[i],&config)<0);
 struct ny_broker_client_s client={"test-plugin",NULL,0};char *response=NULL;
 for(size_t i=0;i<sizeof(g_hardware_routes)/sizeof(g_hardware_routes[0]);i++){
  const struct ny_hardware_route_s *route=&g_hardware_routes[i];
  for(uint64_t bit=1;bit;bit<<=1)if(route->permissions&bit){
   client.permissions=route->permissions&~bit;unsigned previous=calls;
   assert(ny_broker_hardware_request(&client,route->topic,"{}",2,&response)==-EACCES);assert(calls==previous);assert(!response);
  }
  client.permissions=route->permissions;
  int rc=ny_broker_hardware_request(&client,route->topic,"{}",2,&response);
  if(!strcmp(route->topic,"camera.capture")||!strcmp(route->topic,"audio.capture"))assert(rc==-ENOTSUP);
  else{assert(rc==0);assert(response);free(response);response=NULL;}
 }
 client.permissions=UINT64_MAX;unsigned previous=calls;
 assert(ny_broker_hardware_request(&client,"access.create","{}",2,&response)==-EACCES);assert(calls==previous);
 assert(ny_broker_hardware_request(&client,"audio.status","[]",2,&response)==-EINVAL);
 client.permissions=NY_PERMISSION_BLUETOOTH_READ;
 assert(ny_broker_hardware_request(&client,"bt.status","{}",2,&response)==0);
 assert(!strstr(response,"private")&&!strstr(response,"passkey"));free(response);
 client.permissions|=NY_PERMISSION_BLUETOOTH_CALL|NY_PERMISSION_BLUETOOTH_CONNECT;
 assert(ny_broker_hardware_request(&client,"bt.status","{}",2,&response)==0);assert(strstr(response,"private"));free(response);
 client.permissions=NY_PERMISSION_USB_ACCESS;
 assert(ny_broker_hardware_request(&client,"usb.devices","{}",2,&response)==0);
 assert(strstr(response,"usb-audio")&&strstr(response,"usbp0")&&!strstr(response,"pcm0")&&!strstr(response,"cpu"));free(response);
 puts("NY_ICON_HARDWARE_PASS");return 0;
}
''')
    for flags in [['-O2'], ['-O1','-g','-fsanitize=address,undefined']]:
        binary = root/'test'
        subprocess.run(['cc','-std=c11','-D_GNU_SOURCE',*flags,'-I'+str(root),'-I'+str(args.core.resolve()),str(source),str(args.cjson/'cJSON.c'),'-lm','-o',str(binary)],check=True)
        subprocess.run([str(binary),str(root)],check=True)
