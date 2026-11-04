#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import sys,re,tempfile,subprocess
from pathlib import Path
core,cjson=map(Path,sys.argv[1:])
source=(core/'ny_product_device.c').read_text()
body=re.search(r'static cJSON \*ny_product_device_peripherals\(void\)\n\{.*?\n\}',source,re.S)
assert body
code='''#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <cJSON.h>
#define CONFIG_NYABULA_CORE_CAMERA 1
#define CONFIG_NYABULA_CORE_LIGHTING 1
static int nodes;
static int fake_access(const char *path,int mode){assert(mode==F_OK);return ((!strcmp(path,"/dev/video0")&&(nodes&1))||(!strcmp(path,"/dev/ws2812")&&(nodes&2)))?0:-1;}
#define access fake_access
'''+body.group(0)+'''
int main(void){
 for(nodes=0;nodes<4;nodes++){
  cJSON *out=ny_product_device_peripherals();assert(out&&cJSON_GetArraySize(out)==2);
  cJSON *camera=cJSON_GetArrayItem(out,0),*light=cJSON_GetArrayItem(out,1);
  assert(!strcmp(cJSON_GetObjectItem(camera,"kind")->valuestring,"camera"));
  assert(!strcmp(cJSON_GetObjectItem(light,"kind")->valuestring,"lighting"));
  assert(cJSON_IsTrue(cJSON_GetObjectItem(camera,"available"))==!!(nodes&1));
  assert(cJSON_IsTrue(cJSON_GetObjectItem(light,"available"))==!!(nodes&2));cJSON_Delete(out);
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ny-devices-') as temp:
 p=Path(temp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(cjson),str(p/'test.c'),str(cjson/'cJSON.c'),'-lm','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 print('DEVICE_PERIPHERALS_PASS camera lighting registered_and_absent no_device_open')
