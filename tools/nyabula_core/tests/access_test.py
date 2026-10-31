#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise native member credentials and role policy with real mbedTLS."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('core', type=Path)
p.add_argument('mbedtls', type=Path)
p.add_argument('cjson', type=Path)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='ny-access-') as directory:
    root = Path(directory)
    (root / 'nuttx').mkdir()
    (root / 'netutils').mkdir()
    (root / 'nuttx/config.h').write_text('#define CONFIG_CRYPTO_MBEDTLS 1\n')
    (root / 'nuttx/mutex.h').write_text(
        '#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n'
        '#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n'
        '#define nxmutex_lock(m) (-pthread_mutex_lock(m))\n'
        '#define nxmutex_unlock(m) (-pthread_mutex_unlock(m))\n')
    (root / 'netutils/cJSON.h').symlink_to(a.cjson / 'cJSON.h')
    (root / 'mbed-config.h').write_text('#define MBEDTLS_MD_C\n#define MBEDTLS_SHA256_C\n')
    fixture = root / 'access_test.c'
    fixture.write_text(r'''
#include <string.h>
#include <stdio.h>
#include <assert.h>
static size_t test_strlcpy(char *dst, const char *src, size_t size)
{ size_t length = strlen(src); if (size) snprintf(dst, size, "%s", src); return length; }
#define strlcpy test_strlcpy
#include "ny_access.c"
static cJSON *database;
static uint64_t rev;
static bool fail_write;
int ny_product_store_read(const char *domain, cJSON **value, uint64_t *revision)
{ assert(!strcmp(domain,"access")); *value=cJSON_Duplicate(database,true); *revision=rev; return 0; }
int ny_product_store_write(const char *domain, const cJSON *value, uint64_t expected, uint64_t *revision)
{ assert(!strcmp(domain,"access")); if(fail_write)return -EIO; assert(expected==rev); cJSON_Delete(database);database=cJSON_Duplicate(value,true);*revision=++rev;return 0; }
int main(void)
{
  struct ny_product_caller_s owner={"owner",NY_PRODUCT_OWNER,true};
  struct ny_product_caller_s guest={"guest",NY_PRODUCT_GUEST,true};
  cJSON *data=cJSON_Parse("{\"name\":\"Family\",\"role\":\"family\"}");
  cJSON *result=NULL;
  assert(ny_access_request(&guest,"access.create",data,&result)==-EACCES);
  assert(ny_access_request(&owner,"access.create",data,&result)==0);
  char token[65], id[64]; enum ny_product_role_e role; uint32_t generation;
  strlcpy(token,ny_access_string(result,"token"),sizeof(token));
  assert(strlen(token)==64);
  assert(ny_access_auth(token,id,sizeof(id),&role,&generation)==0);
  assert(role==NY_PRODUCT_FAMILY && generation==1);
  cJSON *public_member=cJSON_GetArrayItem(cJSON_GetObjectItem(result,"members"),0);
  assert(cJSON_GetObjectItem(public_member,"digest")==NULL);
  char *stored=cJSON_PrintUnformatted(database);assert(strstr(stored,token)==NULL);free(stored);
  cJSON_Delete(result);result=NULL;cJSON_Delete(data);
  data=cJSON_CreateObject();cJSON_AddStringToObject(data,"memberId",id);cJSON_AddStringToObject(data,"role","guest");
  fail_write=true;
  assert(ny_access_request(&owner,"access.role",data,&result)==-EIO);
  assert(ny_access_generation()==generation);
  assert(ny_access_auth(token,id,sizeof(id),&role,&generation)==0 && role==NY_PRODUCT_FAMILY);
  fail_write=false;
  assert(ny_access_request(&owner,"access.role",data,&result)==0);cJSON_Delete(result);result=NULL;
  assert(ny_access_auth(token,id,sizeof(id),&role,&generation)==0 && role==NY_PRODUCT_GUEST && generation==2);
  assert(!ny_access_allowed(role,"ui.event"));assert(ny_access_allowed(role,"ui.tree"));
  assert(!ny_access_allowed(NY_PRODUCT_FAMILY,"sys.password.set"));
  assert(!ny_access_allowed(NY_PRODUCT_FAMILY,"model.delete"));
  assert(ny_access_allowed(NY_PRODUCT_FAMILY,"ui.event"));
  assert(!ny_access_allowed(NY_PRODUCT_GUEST,"unknown.future.topic"));
  assert(ny_access_request(&owner,"access.revoke",data,&result)==0);cJSON_Delete(result);
  assert(ny_access_auth(token,id,sizeof(id),&role,&generation)==-EACCES);
  cJSON_Delete(data);cJSON_Delete(database);
  puts("NYABULA_ACCESS_PASS");
  return 0;
}
''')
    sources = [fixture, a.cjson / 'cJSON.c'] + [a.mbedtls / ('library/' + name + '.c')
        for name in ['md', 'sha256', 'constant_time', 'platform_util']]
    for label, flags in [('plain', ['-O2']), ('sanitized', ['-g', '-O1', '-fsanitize=address,undefined'])]:
        binary = root / label
        subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', *flags,
            '-include', 'stdlib.h', '-DMBEDTLS_CONFIG_FILE="mbed-config.h"',
            '-I' + str(root), '-I' + str(a.core), '-I' + str(a.mbedtls / 'include'),
            *map(str, sources), '-pthread', '-lm', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
