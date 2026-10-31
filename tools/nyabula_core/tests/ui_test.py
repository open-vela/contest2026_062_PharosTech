#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Test the production UI cache, validation, bindings and generation fences."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('core', type=Path)
p.add_argument('cjson', type=Path)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='ny-ui-') as directory:
    root = Path(directory)
    (root / 'nuttx').mkdir()
    (root / 'netutils').mkdir()
    (root / 'nuttx/config.h').write_text('#define CONFIG_NYABULA_CORE_MAX_PLUGINS 4\n#define CONFIG_NYABULA_CORE_EVENT_SIZE 4096\n')
    (root / 'nuttx/mutex.h').write_text(
        '#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n'
        '#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n'
        '#define nxmutex_lock(m) (-pthread_mutex_lock(m))\n'
        '#define nxmutex_unlock(m) (-pthread_mutex_unlock(m))\n')
    (root / 'netutils/cJSON.h').symlink_to(a.cjson / 'cJSON.h')
    fixture = root / 'ui_test.c'
    fixture.write_text(r'''
#include <string.h>
#include <stdio.h>
#include <assert.h>
static size_t test_strlcpy(char *dst, const char *src, size_t size)
{ size_t length=strlen(src);if(size)snprintf(dst,size,"%s",src);return length; }
#define strlcpy test_strlcpy
#include "ny_ui.c"
static uint64_t granted;
static uint32_t generation=1;
static bool running=true;
static char dispatched[4096];
int ny_permission_apply(struct ny_plugin_config_s *config)
{ config->permissions=config->requested_permissions & granted;return 0; }
int ny_scheduler_status(const char *id, struct ny_scheduler_status_s *status)
{ (void)id;memset(status,0,sizeof(*status));strcpy(status->state,running?"running":"stopped");status->generation=generation;return 0; }
int ny_scheduler_dispatch(const char *id,const char *event)
{(void)id;strlcpy(dispatched,event,sizeof(dispatched));return 0;}
int main(void)
{
 const char *doc="{\"nyaui\":1,\"root\":{\"type\":\"column\",\"children\":[{\"type\":\"slider\",\"id\":\"amount\"},{\"type\":\"button\",\"id\":\"go\",\"on\":{\"tap\":{\"command\":\"feed\",\"args\":{\"$grams\":\"amount\"}}}}]}}";
 struct ny_broker_client_s client={"test",NULL,NY_PERMISSION_UI_PAGE};
 cJSON *tree=NULL;
 assert(ny_ui_publish(&client,1,doc,strlen(doc))==-EACCES);
 granted=NY_PERMISSION_UI_PAGE;
 assert(ny_ui_publish(&client,1,doc,strlen(doc))==0);
 assert(ny_ui_tree("test",&tree)==0);cJSON_Delete(tree);
 cJSON *event=cJSON_Parse("{\"componentId\":\"go\",\"event\":\"tap\",\"values\":{\"amount\":20},\"args\":{\"grams\":999}}");
 assert(ny_ui_event("test",event)==0);
 tree=cJSON_Parse(dispatched);assert(!strcmp(ny_ui_string(tree,"command"),"feed"));
 assert(cJSON_GetObjectItem(cJSON_GetObjectItem(tree,"args"),"grams")->valueint==20);cJSON_Delete(tree);
 cJSON_AddStringToObject(event,"command","injected");assert(ny_ui_event("test",event)==-EACCES);cJSON_Delete(event);
 const char *trailing="{\"nyaui\":1,\"root\":{\"type\":\"text\"}} false";
 assert(ny_ui_publish(&client,1,trailing,strlen(trailing))==-EINVAL);
 const char *duplicate="{\"nyaui\":1,\"root\":{\"type\":\"column\",\"children\":[{\"type\":\"text\",\"id\":\"x\"},{\"type\":\"text\",\"id\":\"x\"}]}}";
 assert(ny_ui_publish(&client,1,duplicate,strlen(duplicate))<0);
 granted=0;assert(ny_ui_tree("test",&tree)==-EACCES);
 granted=NY_PERMISSION_UI_PAGE;generation=2;assert(ny_ui_tree("test",&tree)==-ENOENT);
 ny_ui_clear("test",2);generation=1;assert(ny_ui_tree("test",&tree)==0);cJSON_Delete(tree);
 ny_ui_clear("test",1);assert(ny_ui_tree("test",&tree)==-ENOENT);
 client.permissions=granted=NY_PERMISSION_UI_HTML;
 const char *html="{\"nyaui\":1,\"html\":\"<button>OK</button>\",\"events\":{\"tap\":{\"command\":\"feed\"}}}";
 assert(ny_ui_publish(&client,1,html,strlen(html))==0);
 event=cJSON_Parse("{\"event\":\"tap\"}");assert(ny_ui_event("test",event)==0);cJSON_Delete(event);

 ny_ui_clear("test",0);
 char bundle[2048];snprintf(bundle,sizeof(bundle),"{\"nyaui\":1,\"views\":{\"component\":%s,\"html\":%s}}",doc,html);
 client.permissions=granted=NY_PERMISSION_UI_PAGE;
 assert(ny_ui_publish(&client,1,bundle,strlen(bundle))==-EACCES);
 client.permissions=granted=NY_PERMISSION_UI_PAGE|NY_PERMISSION_UI_HTML;
 assert(ny_ui_publish(&client,1,bundle,strlen(bundle))==0);
 event=cJSON_Parse("{\"view\":\"html\",\"event\":\"tap\"}");assert(ny_ui_event("test",event)==0);cJSON_Delete(event);
 tree=cJSON_Parse(dispatched);assert(!strcmp(ny_ui_string(tree,"view"),"html"));cJSON_Delete(tree);
 event=cJSON_Parse("{\"view\":\"invalid\",\"event\":\"tap\"}");assert(ny_ui_event("test",event)==-EACCES);cJSON_Delete(event);
 running=false;assert(ny_ui_tree("test",&tree)==-ENOENT);ny_ui_clear("test",0);
 puts("NYABULA_UI_PASS");return 0;
}
''')
    for label, flags in [('plain', ['-O2']), ('sanitized', ['-g', '-O1', '-fsanitize=address,undefined'])]:
        binary = root / label
        subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', *flags,
            '-I' + str(root), '-I' + str(a.core), str(fixture),
            str(a.cjson / 'cJSON.c'), '-pthread', '-lm', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
