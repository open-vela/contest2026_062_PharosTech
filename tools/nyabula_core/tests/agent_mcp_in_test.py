#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile actual ingress policy with sanitizers, actual cJSON and libsodium."""
from pathlib import Path
import subprocess
import sys
import tempfile

core, cjson, sodium = map(Path, sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='nyabot-mcp-in-policy-') as directory:
    root = Path(directory)
    (root / 'nuttx').mkdir()
    (root / 'nuttx/mutex.h').write_text(
        '#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n'
        '#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n'
        '#define nxmutex_lock pthread_mutex_lock\n#define nxmutex_unlock pthread_mutex_unlock\n')
    (root / 'netutils').mkdir()
    (root / 'netutils/cJSON.h').symlink_to((cjson / 'cJSON.h').resolve())
    (root / 'test.c').write_text(r'''
#include "ny_agent_mcp_in.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static cJSON *saved;
static uint64_t revision;
static uint64_t now = 1800000000000ULL;
static int reads;
static int fail_read, fail_write;
static const char *token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const struct ny_product_caller_s owner = {"nsh", NY_PRODUCT_OWNER, true};
static const struct ny_product_caller_s web = {"web", NY_PRODUCT_OWNER, false};
static const struct ny_product_caller_s guest = {"guest", NY_PRODUCT_GUEST, true};
uint64_t ny_product_time_ms(bool monotonic) {(void)monotonic; return now;}
unsigned int ny_mcp_in_listener(void) {return 0;}
int ny_agent_remote_request(const char *principal, const char *topic,
                             const cJSON *data, cJSON **result) {
  (void)principal; (void)topic; (void)data; (void)result;
  return -EACCES;
}
int ny_product_store_read(const char *domain, cJSON **root, uint64_t *rev) {
  assert(!strcmp(domain, "mcp-in"));
  if (fail_read) return -EIO;
  *root = saved ? cJSON_Duplicate(saved, true) : NULL;
  *rev = revision; return 0;
}
int ny_product_store_write(const char *domain, const cJSON *root, uint64_t expected, uint64_t *rev) {
  assert(!strcmp(domain, "mcp-in"));
  if (expected != revision) return -ESTALE;
  if (fail_write) return -EIO;
  cJSON_Delete(saved); saved = cJSON_Duplicate(root, true);
  *rev = ++revision; return 0;
}
int ny_product_request(const struct ny_product_caller_s *caller, const char *topic,
                       const cJSON *data, cJSON **result) {
  assert(caller->role == NY_PRODUCT_FAMILY && !caller->local_transport);
  assert(!strcmp(topic, "system.time.get"));
  assert(cJSON_IsObject(data) && !data->child);
  reads++;
  *result = cJSON_Parse("{\"clock_valid\":true}");
  return 0;
}
static int api(const struct ny_product_caller_s *caller, const char *topic, const char *json) {
  cJSON *data = cJSON_Parse(json), *result = NULL;
  int ret = ny_agent_mcp_in(caller, topic, data, &result);
  if (result) {
    char *text = cJSON_PrintUnformatted(result);
    assert(!strstr(text, token) && !strstr(text, "\"hash\""));
    free(text);
  }
  cJSON_Delete(data); cJSON_Delete(result); return ret;
}
static int rpc(const char *credential, const char *json, bool error) {
  cJSON *request = cJSON_Parse(json), *reply = NULL;
  int status = ny_mcp_in_rpc(credential, request, &reply);
  if (reply) {
    cJSON *result = cJSON_GetObjectItemCaseSensitive(reply, "result");
    cJSON *is_error = cJSON_GetObjectItemCaseSensitive(result, "isError");
    if (is_error) assert(cJSON_IsTrue(is_error) == error);
    char *text = cJSON_PrintUnformatted(reply);
    assert(!strstr(text, token) && !strstr(text, "\"hash\""));
    free(text);
  }
  cJSON_Delete(request); cJSON_Delete(reply); return status;
}
int main(void) {
  const char *call = "{\"jsonrpc\":\"2.0\",\"id\":2147483659,\"method\":\"tools/call\",\"params\":{\"name\":\"nyabula_time\",\"arguments\":{}}}";
  const char *save = "{\"revision\":1,\"id\":\"one\",\"title\":\"One\",\"expiresAt\":1800000005000,\"scopes\":[\"nyabula_time\"],\"token\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}";
  assert(api(&guest,"agent.mcp.in.list","{}") == -EACCES);
  assert(api(&owner,"agent.mcp.in.enable","{\"revision\":0,\"enabled\":true}") == 0);
  assert(api(&web,"agent.mcp.in.save",save) == -EACCES);
  assert(api(&owner,"agent.mcp.in.save",save) == 0);
  assert(api(&owner,"agent.mcp.in.save",save) == -ESTALE);
  assert(rpc(NULL,call,false) == 401);
  assert(rpc(token,call,false) == 200 && reads == 1);
  const char *bad[] = {
    "null","[]","{}", "{\"jsonrpc\":\"2.0\",\"id\":true,\"method\":\"ping\"}",
    "{\"jsonrpc\":\"2.0\",\"id\":1.2,\"method\":\"ping\"}",
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":null}",
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"agent.approval.decide\"}}",
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"memory.list\"}}",
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"nyabula_time\",\"arguments\":{\"topic\":\"shell\"}}}",
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"nyabula_time\",\"arguments\":[]}}"
  };
  for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) assert(rpc(token,bad[i],true)==200);
  assert(reads==1);
  fail_write = 1;
  assert(rpc(token,call,false)==500);
  fail_write = 0;
  assert(reads==2);
  fail_read = 1;
  assert(rpc(token,call,false)==503);
  fail_read = 0;
  now += 6000;
  assert(rpc(token,call,false)==401 && reads==2);
  now = 1;
  assert(rpc(token,call,false)==401 && reads==2);
  now = 1800000000000ULL;
  char buf[512];
  snprintf(buf,sizeof(buf),"{\"revision\":%llu,\"id\":\"one\",\"title\":\"One\",\"expiresAt\":1800000005000,\"scopes\":[]}",(unsigned long long)revision);
  assert(api(&web,"agent.mcp.in.save",buf)==0);
  assert(rpc(token,call,true)==200 && reads==2);
  snprintf(buf,sizeof(buf),"{\"revision\":%llu,\"id\":\"one\"}",(unsigned long long)revision);
  assert(api(&web,"agent.mcp.in.delete",buf)==0);
  assert(rpc(token,call,false)==401);
  cJSON_Delete(saved);
  puts("PASS: ingress local-secret guard, exact scopes, non-owner reads, redaction, expiry, revoke, CAS, storage fail-closed");
}
''')
    binary = root / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g', '-no-pie',
                    '-fsanitize=address,undefined', '-I'+str(root), '-I'+str(core),
                    '-I'+str(sodium), str(root/'test.c'), str(core/'ny_agent_mcp_in.c'),
                    str(cjson/'cJSON.c'), '-pthread', '-lm', '-l:libsodium.so.23', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
