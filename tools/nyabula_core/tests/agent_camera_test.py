#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the real builtin dispatch: camera capture is an approval-gated tool."""
import argparse
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('core', type=Path)
parser.add_argument('cjson', type=Path)
args = parser.parse_args()
names = ['tool_read_file_execute', 'tool_list_dir_execute', 'tool_write_file_execute',
         'tool_edit_file_execute', 'tool_run_shell_execute', 'tool_web_search_execute',
         'tool_news_search_execute', 'tool_get_weather_execute', 'tool_fetch_url_execute']
code = r'''
#include <assert.h>
#include <stdio.h>
#include "ny_agent_builtin.c"
static int captured, uploaded, capture_error;
static bool ready=true;
int claw_config_get(const char *key,char *value,size_t size){(void)key;(void)value;(void)size;return -1;}
int claw_config_set_many(const char *const *keys,const char *const *values,size_t count){(void)keys;(void)values;(void)count;return -1;}
int tool_web_search_init(void){return 0;}
int ny_mimo_vision_tool(const char *in,char *out,size_t size){(void)in;(void)out;(void)size;return -ENOSYS;}
int ny_mimo_config(const char *topic,const cJSON *data,cJSON **result){(void)data;assert(!strcmp(topic,"agent.config.mimo.get"));*result=cJSON_CreateObject();cJSON_AddBoolToObject(*result,"credentialsReady",ready);return 0;}
int ny_product_camera_photo(unsigned int factor,uint8_t **out,size_t *length){assert(factor==2);captured++;if(capture_error)return capture_error;*out=malloc(8);memcpy(*out,"\x89PNG\r\n\x1a\n",8);*length=8;return 0;}
int ny_mimo_vision_png(const uint8_t *png,size_t length,const char *prompt,char *output,size_t capacity){assert(length==8&&!memcmp(png,"\x89PNG\r\n\x1a\n",8));assert(!strcmp(prompt,"What is here?"));uploaded++;snprintf(output,capacity,"A test pattern");return 0;}
''' + ''.join(f'int {n}(const char *in,char *out,size_t size){{(void)in;(void)out;(void)size;return -ENOSYS;}}\n' for n in names) + r'''
int main(void){
 cJSON *out=NULL;assert(ny_agent_builtin("agent.tools.catalog",NULL,&out)==0);
 cJSON *items=cJSON_GetObjectItem(out,"items"),*item;bool found=false;
 cJSON_ArrayForEach(item,items){if(!strcmp(cJSON_GetObjectItem(item,"name")->valuestring,"camera_vision")){found=true;assert(cJSON_IsTrue(cJSON_GetObjectItem(item,"approval")));}}
 assert(found);cJSON_Delete(out);out=NULL;
 cJSON *data=cJSON_Parse("{\"name\":\"camera_vision\",\"arguments\":{\"prompt\":\"What is here?\"}}");
 assert(ny_agent_builtin("agent.tools.read",data,&out)==-EACCES);assert(captured==0&&uploaded==0);
 ready=false;assert(ny_agent_builtin("agent.tools.call",data,&out)==-EIO);cJSON_Delete(out);out=NULL;assert(captured==0&&uploaded==0);
 ready=true;capture_error=-ENODEV;assert(ny_agent_builtin("agent.tools.call",data,&out)==-EIO);cJSON_Delete(out);out=NULL;assert(captured==1&&uploaded==0);
 capture_error=0;assert(ny_agent_builtin("agent.tools.call",data,&out)==0);assert(captured==2&&uploaded==1);
 assert(!strcmp(cJSON_GetObjectItem(out,"text")->valuestring,"A test pattern"));char *encoded=cJSON_PrintUnformatted(out);assert(!strstr(encoded,"base64")&&!strstr(encoded,"image/png"));free(encoded);cJSON_Delete(out);out=NULL;
 cJSON *arguments=cJSON_GetObjectItem(data,"arguments");cJSON_AddStringToObject(arguments,"url","file:///private");
 assert(ny_agent_builtin("agent.tools.call",data,&out)==-EIO);assert(captured==2&&uploaded==1);cJSON_Delete(out);cJSON_Delete(data);
 puts("AGENT_CAMERA_PASS approval_catalog read_path_denied credentials_before_capture failure_no_upload text_only_history");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ny-agent-camera-') as directory:
    root = Path(directory)
    for folder in ['nuttx', 'netutils', 'infra', 'tools']:
        (root/folder).mkdir()
    (root/'nuttx/config.h').write_text('#define CONFIG_NYABULA_CORE_CAMERA 1\n')
    (root/'nuttx/mutex.h').write_text('#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n#define nxmutex_lock pthread_mutex_lock\n#define nxmutex_unlock pthread_mutex_unlock\n')
    (root/'netutils/cJSON.h').write_text('#include <cJSON.h>\n')
    (root/'agent_config.h').write_text('#define AGENT_DATA_DIR "/unused-test"\n#define AGENT_SHELL_SECURITY 0\n' +
        ''.join(f'#define AGENT_CFG_KEY_{key}_KEY "{key}"\n' for key in ['SERP', 'EXA', 'TAVILY', 'NEWS']))
    (root/'infra/config_store.h').write_text('#include <stddef.h>\nint claw_config_get(const char*,char*,size_t);\nint claw_config_set_many(const char *const*,const char *const*,size_t);\n')
    declarations = '#include <stddef.h>\nint tool_web_search_init(void);\n' + ''.join(f'int {name}(const char*,char*,size_t);\n' for name in names)
    for name in ['tool_fetch_url.h', 'tool_files.h', 'tool_shell.h', 'tool_web_search.h']:
        (root/'tools'/name).write_text(declarations)
    (root/'test.c').write_text(code)
    for name, flags in [('plain', ['-O2']), ('sanitized', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'])]:
        binary = root/name
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Wno-sign-compare', '-Werror', *flags,
                        '-I'+str(root), '-I'+str(args.core), '-I'+str(args.cjson),
                        str(root/'test.c'), str(args.cjson/'cJSON.c'), '-pthread', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
