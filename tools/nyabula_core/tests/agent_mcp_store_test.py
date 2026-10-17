#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise actual MCP policy code with an in-memory CAS store and fake peer."""
import pathlib
import subprocess
import sys
import tempfile

core, cjson = map(pathlib.Path, sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='nyabot-mcp-policy-') as directory:
    root = pathlib.Path(directory)
    (root / 'nuttx').mkdir()
    (root / 'nuttx/mutex.h').write_text(
        '#include <pthread.h>\ntypedef pthread_mutex_t mutex_t;\n'
        '#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER\n'
        '#define nxmutex_lock pthread_mutex_lock\n'
        '#define nxmutex_unlock pthread_mutex_unlock\n')
    (root / 'netutils').mkdir()
    (root / 'netutils/cJSON.h').symlink_to((cjson / 'cJSON.h').resolve())
    (root / 'test.c').write_text(r'''
#include "ny_agent_mcp.h"
#include "ny_agent_mcp_wire.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static cJSON *stored;
static uint64_t rev;
static int calls, change, revoke, fail;
static const struct ny_product_caller_s owner = {"owner", NY_PRODUCT_OWNER, true};
static const struct ny_product_caller_s web = {"web", NY_PRODUCT_OWNER, false};
static const struct ny_product_caller_s guest = {"guest", NY_PRODUCT_GUEST, true};
int ny_product_store_read(const char *domain, cJSON **value, uint64_t *revision) {
  assert(!strcmp(domain, "mcp-out"));
  *value = stored ? cJSON_Duplicate(stored, true) : NULL;
  *revision = rev; return 0;
}
int ny_product_store_write(const char *domain, const cJSON *value,
                           uint64_t expected, uint64_t *revision) {
  assert(!strcmp(domain, "mcp-out"));
  if (expected != rev) return -ESTALE;
  cJSON_Delete(stored); stored = cJSON_Duplicate(value, true);
  *revision = ++rev; return 0;
}
int ny_mcp_peer_configure(struct ny_mcp_peer_s *peer, const char *url, const char *secret) {
  (void)peer; (void)secret;
  return strncmp(url, "https://", 8) ? -EINVAL : 0;
}
int ny_mcp_peer_initialize(struct ny_mcp_peer_s *peer) { (void)peer; return 0; }
int ny_mcp_peer_catalog(struct ny_mcp_peer_s *peer, cJSON **catalog) {
  (void)peer;
  *catalog = cJSON_Parse(change ?
    "{\"tools\":[{\"name\":\"send\",\"inputSchema\":{\"type\":\"array\"}}],\"resources\":[],\"prompts\":[]}" :
    "{\"tools\":[{\"name\":\"send\",\"inputSchema\":{\"type\":\"object\"}}],\"resources\":[],\"prompts\":[]}");
  if (revoke) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"id\":\"sample\",\"revision\":%llu,\"generation\":2,\"grants\":[]}", (unsigned long long)rev);
    cJSON *data = cJSON_Parse(buf), *out = NULL;
    assert(ny_agent_mcp(&owner, "agent.mcp.out.grant", data, &out) == 0);
    cJSON_Delete(data); cJSON_Delete(out);
  }
  return 0;
}
int ny_mcp_peer_request(struct ny_mcp_peer_s *peer, const char *method,
                        const cJSON *params, cJSON **result) {
  (void)peer;
  assert(!strcmp(method, "tools/call"));
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(params,"name")->valuestring,"send"));
  calls++;
  if (fail == 1) return -EIO;
  *result = cJSON_Parse(fail == 2 ? "{\"isError\":true}" : "{\"content\":[]}");
  return 0;
}
static int api(const struct ny_product_caller_s *caller, const char *topic, const char *json) {
  cJSON *data = cJSON_Parse(json), *result = NULL;
  int ret = ny_agent_mcp(caller, topic, data, &result);
  if (result) {
    char *text = cJSON_PrintUnformatted(result);
    assert(!strstr(text, "topsecret"));
    assert(!strstr(text, "\"secret\"")); free(text);
  }
  cJSON_Delete(data); cJSON_Delete(result); return ret;
}
static int call(bool *uncertain) {
  cJSON *data = cJSON_Parse("{\"server\":\"sample\",\"kind\":\"tools\",\"name\":\"send\",\"generation\":2,\"arguments\":{\"text\":\"hello\"}}");
  cJSON *result = NULL;
  int ret = ny_agent_mcp_call(data, &result, uncertain);
  cJSON_Delete(data); cJSON_Delete(result); return ret;
}
int main(void) {
  const char *save = "{\"id\":\"sample\",\"title\":\"Sample\",\"url\":\"https://sample.test/mcp\",\"secret\":\"topsecret\",\"revision\":0}";
  assert(api(&guest,"agent.mcp.out.list","{}") == -EACCES);
  assert(api(&web,"agent.mcp.out.save",save) == -EACCES);
  assert(api(&owner,"agent.mcp.out.save",save) == 0);
  assert(api(&owner,"agent.mcp.out.save",save) == -ESTALE);
  assert(api(&web,"agent.mcp.out.save","{\"id\":\"sample\",\"title\":\"Sample\",\"url\":\"https://evil.test/mcp\",\"revision\":1}") == -EACCES);
  assert(api(&web,"agent.mcp.out.list","{}") == 0);
  bool uncertain = true;
  assert(call(&uncertain) == -EACCES && !uncertain && !calls);
  assert(api(&owner,"agent.mcp.out.refresh","{\"id\":\"sample\",\"revision\":1}") == 0);
  assert(api(&owner,"agent.mcp.out.grant","{\"id\":\"sample\",\"revision\":2,\"generation\":2,\"grants\":[{\"kind\":\"tools\",\"name\":\"unknown\"}]}") == -ENOENT);
  assert(api(&owner,"agent.mcp.out.grant","{\"id\":\"sample\",\"revision\":2,\"generation\":2,\"grants\":[{\"kind\":\"tools\",\"name\":\"send\"}]}") == 0);
  assert(call(&uncertain) == 0 && !uncertain && calls == 1);
  fail = 1;
  assert(call(&uncertain) == -EIO && uncertain && calls == 2);
  fail = 2;
  assert(call(&uncertain) == -EREMOTEIO && uncertain && calls == 3);
  fail = 0; change = 1;
  assert(call(&uncertain) == -ESTALE && !uncertain && calls == 3);
  change = 0; revoke = 1;
  assert(call(&uncertain) == -ESTALE && !uncertain && calls == 3);
  revoke = 0;
  assert(call(&uncertain) == -EACCES && !uncertain && calls == 3);
  assert(api(&owner,"agent.mcp.out.refresh","{\"id\":\"sample\",\"revision\":4}") == 0);
  assert(call(&uncertain) == -EACCES && !uncertain && calls == 3);
  cJSON_Delete(stored);
  puts("PASS: MCP owner/local-secret guards, redaction, CAS, exact grants, schema drift, concurrent revocation, uncertain effects, no retry");
}
''')
    binary = root / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(root), '-I' + str(core), str(root / 'test.c'),
                    str(core / 'ny_agent_mcp.c'), str(cjson / 'cJSON.c'),
                    '-pthread', '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
