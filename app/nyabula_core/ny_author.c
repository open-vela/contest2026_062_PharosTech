/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_author.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agent_config.h"
#include "infra/config_store.h"
#include "node/node_client.h"
#include "ny_author.h"
#include "ny_package.h"
#include "ny_permission.h"
#include "ny_product_store.h"
#include "ny_scheduler.h"
#include "ny_state.h"
#include "ny_ui.h"

#define NY_AUTHOR_SOURCE_MAX      8192
#define NY_AUTHOR_TEST_TIMEOUT_MS 30000
#define NY_AUTHOR_ROOT            CONFIG_NYABULA_CORE_PACKAGE_STORE "-authoring"
#define NY_AUTHOR_KEY_ID          "nyabula-device-author"
#define NY_AUTHOR_PREFIX          "agent-"

static const char *const g_author_permissions[] = {
  "core.log",   "storage.read", "storage.write", "ui.page",
  "ui.html",    "ui.notify",    "audio.read",    "bluetooth.read",
  "usb.access", "weather.read", "device.read"
};
static const char *const g_author_nas_permissions[] = { "nas.read",
                                                        "nas.write",
                                                        "nas.manage" };

static const char g_author_wrapper[] =
    "import plugin from './source.js';\n"
    "export async function ny_on_start(){return plugin.onStart?.();}\n"
    "export async function ny_on_event(event){\n"
    "let msg;try{msg=JSON.parse(event)}catch{}\n"
    "if(msg?.type==='ui.event'&&typeof plugin.onCommand==='function')\n"
    "return plugin.onCommand(msg.command,msg.args??{},msg.values??{},msg);\n"
    "return plugin.onEvent?.(event);}\n"
    "export async function ny_on_stop(){return plugin.onStop?.();}\n";

static const char g_author_contract[] =
    "Create useful sandboxed JavaScript plugins on this device. First inspect "
    "an existing locally authored plugin before editing. list returns only "
    "locally authored tools; do not inspect or replace third-party packages. "
    "deploy requires id beginning agent-, "
    "name, source, permissions array, baseVersion (empty for first install). "
    "The host assigns version, signs locally, starts with existing private "
    "data, verifies onStart and a published UI, then activates. Failed starts "
    "restore previous code and private data. Check returned UI and exercise "
    "events with test; inspect before revising. Do not claim an untested "
    "feature works. Preserve stored data formats when iterating.\n"
    "source: one ES module, plain JavaScript, max 8192 UTF-8 bytes; export "
    "default {onStart(),onCommand(command,args,values,event),onStop()}. "
    "No TypeScript syntax, npm, Node, DOM, fetch, TextEncoder, Intl or "
    "timers. "
    "For UTF-8 byte length use unescape(encodeURIComponent(text)).length. Use "
    "Date.now() "
    "for timestamps; refresh through a declared UI action.\n"
    "Imports: import * as storage from '@nyabula/storage'; "
    "get(key) returns string|null, put(key,string). JSON encode records. "
    "import * as ui from '@nyabula/ui'; ui.render(document), "
    "ui.notify(text), ui.eye({action,params,lease_ms}). Notifications are eye "
    "captions: use at most 47 UTF-8 bytes; put detailed messages in the page. "
    "import * as core from '@nyabula/core'; core.log(text). "
    "Read-only hardware: import * as hardware from '@nyabula/hardware'; "
    "await "
    "hardware.request('audio.status'|'bt.status'|'bt.devices'|'usb.devices',{}"
    "). "
    "Declare each permission. Other hardware changes require owner grants, "
    "never self-grant or use shell.\n"
    "Document: "
    "{nyaui:1,page:{title,icon:'extension',placement:'menu'},root:node}. "
    "node={type,id,props,children,on}. Containers: "
    "column,row,grid,card,section. "
    "text props {text}; statCard props {label,value,unit,icon}; "
    "button props {text,variant:'filled'}, on:{tap:{command:'save'}}; "
    "textField props {label,value,placeholder}, id:'input'; use values.input "
    "in onCommand. Inputs include switch {value}, slider "
    "{value,min,max,step}. "
    "All events must be declared; args with '$name':'input' binds input value "
    "to args.name. At most 200 nodes, 12 levels, 4096 bytes per string. "
    "Use Chinese labels and meaningful empty states.\n"
    "Expected invalid form input must render a helpful message and keep the "
    "plugin running, not throw an exception. Storage limits are returned in "
    "this contract; split or bound saved records accordingly.\n"
    "Additional read-only hardware topics: sys.info/device.status require "
    "device.read; "
    "weather.query({location:'city',section:'now'|'daily'|'hourly'}) "
    "requires weather.read. Respect source attribution and data timestamps.\n"
    "If the contract permissions include NAS: nas.status and "
    "nas.list({path:''}) "
    "require nas.read; nas.devices requires usb.access; nas.configure "
    "({revision,enabled,readOnly,device:'auto'|'/dev/sda'}) requires "
    "nas.manage; "
    "nas.mkdir({path:'folder'}) requires nas.write. Read status before "
    "changes. "
    "No format or overwrite operation exists. onStart may only read and "
    "render; "
    "NAS mutations are denied during startup and belong in onCommand. "
    "Never switch readOnly off unless the owner asks. File transfer uses the "
    "owner-authenticated built-in USB files page, not a plugin credential.\n"
    "Optional HTML: "
    "{nyaui:1,page:{title},html:'...',events:{save:{command:'save'}}}. "
    "Sandbox HTML calls nyabula.send('save',{field:'value'}); onCommand gets "
    "values.field. Escape all user text before inserting HTML. Declare "
    "ui.html. "
    "Do not embed external assets. component UI is preferred for forms.\n"
    "test waits for the event handler and reports runtime errors; after an "
    "error "
    "inspect and repair the source before testing again. "
    "test requires id and event {componentId,event:'tap',values:{...}}; "
    "values may only override actual input IDs in the published component UI; "
    "omitted inputs use their published defaults. Click unit buttons before "
    "calculating; do not inject hidden state as imaginary input fields. "
    "HTML uses {view:'html',event:'save',values:{...}}. Testing changes only "
    "this plugin's private records; use explicit test entries. Inspect source "
    "and UI after testing, then fix via deploy with current baseVersion.";

static const char *ny_author_string(const cJSON *data, const char *key);
static bool ny_author_id(const char *id);
static int ny_author_read(const char *path, size_t limit, char **text);
static int ny_author_write(const char *path, const void *bytes, size_t size);
static int ny_author_directory(const char *path);
static int ny_author_join(char *path, size_t capacity, const char *root,
                          const char *name);
static int ny_author_policy(cJSON **policy, uint64_t *revision);
static int ny_author_identity(unsigned char *secret);
static int ny_author_sign(const char *root, const char *manifest,
                          const char *source);
static int ny_author_inspect(const char *id, bool source, cJSON **result);
static int ny_author_visit(const char *id, const char *version,
                           const char *path, bool current, bool last_good,
                           void *opaque);
static int ny_author_journal(const cJSON *value);
static int ny_author_deploy(const cJSON *data, cJSON **result);
static int ny_author_test(const cJSON *data, cJSON **result);
static int ny_author_inputs(const cJSON *node, cJSON *values, cJSON *fields,
                            const cJSON *overrides);
static int ny_author_test_event(const char *id, const cJSON *event,
                                char *diagnostic, size_t capacity);
static void ny_author_cleanup(const char *root);

/****************************************************************************
 * Name: ny_author_join
 ****************************************************************************/

static int ny_author_join(char *path, size_t capacity, const char *root,
                          const char *name)
{
  size_t left = strlen(root), right = strlen(name);
  if (left >= capacity || right >= capacity - left ||
      left + right + 2 > capacity)
    return -ENAMETOOLONG;
  memcpy(path, root, left);
  path[left] = '/';
  memcpy(path + left + 1, name, right + 1);
  return 0;
}

/****************************************************************************
 * Name: ny_author_cleanup
 ****************************************************************************/

static void ny_author_cleanup(const char *root)
{
  static const char *const names[] = { "manifest.json", "source.js",
                                       "main.js",       "signer.json",
                                       "hashes.json",   "signature.ed25519" };
  char path[PATH_MAX];
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    if (snprintf(path, sizeof(path), "%s/%s", root, names[i]) <
        (int)sizeof(path))
      unlink(path);
  rmdir(root);
}

/****************************************************************************
 * Name: ny_author_string
 ****************************************************************************/

static const char *ny_author_string(const cJSON *data, const char *key)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(data, key);
  return cJSON_IsString(value) ? value->valuestring : "";
}

/****************************************************************************
 * Name: ny_author_id
 ****************************************************************************/

static bool ny_author_id(const char *id)
{
  if (strncmp(id, NY_AUTHOR_PREFIX, strlen(NY_AUTHOR_PREFIX)) ||
      strlen(id) <= strlen(NY_AUTHOR_PREFIX) ||
      strlen(id) >= NY_PLUGIN_ID_SIZE)
    return false;
  for (const char *p = id; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
      return false;
  return true;
}

/****************************************************************************
 * Name: ny_author_read
 ****************************************************************************/

static int ny_author_read(const char *path, size_t limit, char **text)
{
  *text = NULL;
  struct stat st;
  if (lstat(path, &st) < 0)
    return -errno;
  if (!S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size > limit)
    return -EFBIG;
  FILE *file = fopen(path, "rb");
  if (!file)
    return -errno;
  char *value = calloc(1, (size_t)st.st_size + 1);
  int ret = !value                                                    ? -ENOMEM
            : fread(value, 1, st.st_size, file) != (size_t)st.st_size ? -EIO
                                                                      : 0;
  fclose(file);
  if (ret < 0)
    free(value);
  else
    *text = value;
  return ret;
}

/****************************************************************************
 * Name: ny_author_write
 ****************************************************************************/

static int ny_author_write(const char *path, const void *bytes, size_t size)
{
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return -errno;
  size_t offset = 0;
  int ret = 0;
  while (offset < size)
    {
      ssize_t count = write(fd, (const char *)bytes + offset, size - offset);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        {
          ret = count < 0 ? -errno : -EIO;
          break;
        }
      offset += count;
    }
  if (ret == 0 && fsync(fd) < 0)
    ret = -errno;
  if (close(fd) < 0 && ret == 0)
    ret = -errno;
  if (ret < 0)
    unlink(path);
  return ret;
}

/****************************************************************************
 * Name: ny_author_directory
 ****************************************************************************/

static int ny_author_directory(const char *path)
{
  char copy[PATH_MAX];
  if (path[0] != '/' ||
      snprintf(copy, sizeof(copy), "%s/", path) >= (int)sizeof(copy))
    return -EINVAL;
  for (char *p = copy + 1; *p; p++)
    if (*p == '/')
      {
        struct stat st;
        *p = '\0';
        if (mkdir(copy, 0700) < 0 && errno != EEXIST)
          return -errno;
        if (lstat(copy, &st) < 0 || !S_ISDIR(st.st_mode))
          return -ENOTDIR;
        *p = '/';
      }
  return 0;
}

/****************************************************************************
 * Name: ny_author_policy
 ****************************************************************************/

static int ny_author_policy(cJSON **policy, uint64_t *revision)
{
  int ret = ny_product_store_read("plugin-author", policy, revision);
  if (ret < 0)
    return ret;
  if (!*policy)
    {
      *policy = cJSON_CreateObject();
      if (!*policy || !cJSON_AddBoolToObject(*policy, "enabled", false))
        return -ENOMEM;
    }
  if (!cJSON_GetObjectItemCaseSensitive(*policy, "nas") &&
      !cJSON_AddBoolToObject(*policy, "nas", false))
    return -ENOMEM;
  return cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(*policy, "enabled")) &&
                 cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(*policy, "nas"))
             ? 0
             : -EBADMSG;
}

/****************************************************************************
 * Name: ny_author_identity
 * Description: Create a device-only signing identity, preserving other keys.
 ****************************************************************************/

static int ny_author_identity(unsigned char *secret)
{
  unsigned char seed[crypto_sign_SEEDBYTES];
  unsigned char public_key[crypto_sign_PUBLICKEYBYTES];
  char encoded[crypto_sign_SEEDBYTES * 2 + 1] = { 0 };
  char public_text[crypto_sign_PUBLICKEYBYTES * 2 + 1];
  if (sodium_init() < 0 || config_store_init() != 0)
    return -EIO;
  claw_config_get("plugin_author_seed", encoded, sizeof(encoded));
  if (!encoded[0])
    {
      if (agent_secure_random(seed, sizeof(seed)) != 0)
        return -EIO;
      sodium_bin2hex(encoded, sizeof(encoded), seed, sizeof(seed));
      if (claw_config_set("plugin_author_seed", encoded) != 0)
        {
          sodium_memzero(seed, sizeof(seed));
          sodium_memzero(encoded, sizeof(encoded));
          return -EIO;
        }
    }
  size_t length = 0;
  int ret = sodium_hex2bin(seed, sizeof(seed), encoded, strlen(encoded), NULL,
                           &length, NULL);
  sodium_memzero(encoded, sizeof(encoded));
  if (ret != 0 || length != sizeof(seed))
    {
      sodium_memzero(seed, sizeof(seed));
      return -EBADMSG;
    }
  crypto_sign_seed_keypair(public_key, secret, seed);
  sodium_memzero(seed, sizeof(seed));
  sodium_bin2hex(public_text, sizeof(public_text), public_key,
                 sizeof(public_key));
  char *text = NULL;
  ret = ny_author_read(CONFIG_NYABULA_CORE_TRUST_STORE, 65536, &text);
  cJSON *trust = ret == -ENOENT ? cJSON_CreateObject()
                 : ret == 0     ? cJSON_Parse(text)
                                : NULL;
  free(text);
  if (!cJSON_IsObject(trust))
    {
      cJSON_Delete(trust);
      return ret < 0 && ret != -ENOENT ? ret : -EBADMSG;
    }
  const cJSON *existing =
      cJSON_GetObjectItemCaseSensitive(trust, NY_AUTHOR_KEY_ID);
  if (existing)
    {
      ret = cJSON_IsString(existing) &&
                    !strcmp(existing->valuestring, public_text)
                ? 0
                : -EACCES;
      cJSON_Delete(trust);
      return ret;
    }
  if (!cJSON_AddStringToObject(trust, NY_AUTHOR_KEY_ID, public_text))
    {
      cJSON_Delete(trust);
      return -ENOMEM;
    }
  text = cJSON_PrintUnformatted(trust);
  cJSON_Delete(trust);
  if (!text)
    return -ENOMEM;
  char temporary[PATH_MAX];
  snprintf(temporary, sizeof(temporary), "%s.author-tmp",
           CONFIG_NYABULA_CORE_TRUST_STORE);
  /* This fixed path belongs only to the serialized authoring transaction. */
  unlink(temporary);
  ret = ny_author_write(temporary, text, strlen(text));
  free(text);
  if (ret == 0 && rename(temporary, CONFIG_NYABULA_CORE_TRUST_STORE) < 0)
    ret = -errno;
  if (ret < 0)
    unlink(temporary);
  return ret;
}

/****************************************************************************
 * Name: ny_author_sign
 ****************************************************************************/

static int ny_author_sign(const char *root, const char *manifest,
                          const char *source)
{
  static const char signer[] = "{\"keyId\":\"" NY_AUTHOR_KEY_ID "\"}";
  const char *names[] = { "manifest.json", "source.js", "main.js",
                          "signer.json" };
  const char *contents[] = { manifest, source, g_author_wrapper, signer };
  unsigned char secret[crypto_sign_SECRETKEYBYTES] = { 0 };
  unsigned char signature[crypto_sign_BYTES];
  cJSON *hashes = cJSON_CreateObject();
  int ret = hashes ? ny_author_identity(secret) : -ENOMEM;
  char path[PATH_MAX];
  char *encoded = NULL;
  for (size_t i = 0; ret == 0 && i < 4; i++)
    {
      unsigned char digest[crypto_hash_sha256_BYTES];
      char hex[crypto_hash_sha256_BYTES * 2 + 1];
      size_t size = strlen(contents[i]);
      crypto_hash_sha256(digest, (const unsigned char *)contents[i], size);
      sodium_bin2hex(hex, sizeof(hex), digest, sizeof(digest));
      if (!cJSON_AddStringToObject(hashes, names[i], hex))
        ret = -ENOMEM;
      else if (snprintf(path, sizeof(path), "%s/%s", root, names[i]) >=
               (int)sizeof(path))
        ret = -ENAMETOOLONG;
      else
        ret = ny_author_write(path, contents[i], size);
    }
  if (ret == 0)
    {
      encoded = cJSON_PrintUnformatted(hashes);
      ret = !encoded ? -ENOMEM
            : crypto_sign_detached(signature, NULL,
                                   (const unsigned char *)encoded,
                                   strlen(encoded), secret)
                ? -EIO
                : 0;
    }
  if (ret == 0)
    {
      ret = ny_author_join(path, sizeof(path), root, "hashes.json");
      if (ret == 0)
        ret = ny_author_write(path, encoded, strlen(encoded));
    }
  if (ret == 0)
    {
      ret = ny_author_join(path, sizeof(path), root, "signature.ed25519");
      if (ret == 0)
        ret = ny_author_write(path, signature, sizeof(signature));
    }
  sodium_memzero(secret, sizeof(secret));
  free(encoded);
  cJSON_Delete(hashes);
  return ret;
}

/****************************************************************************
 * Name: ny_author_inspect
 ****************************************************************************/

static int ny_author_inspect(const char *id, bool source, cJSON **result)
{
  if (!ny_author_id(id))
    return -EINVAL;
  char path[PATH_MAX];
  char file[PATH_MAX];
  struct ny_plugin_config_s config;
  int ret = ny_package_resolve(id, path, sizeof(path));
  if (ret == 0)
    ret = ny_manifest_load(path, &config);
  if (ret < 0)
    return ret;
  ret = ny_author_join(file, sizeof(file), path, "signer.json");
  if (ret < 0)
    return ret;
  char *text = NULL;
  ret = ny_author_read(file, 1024, &text);
  cJSON *signer = text ? cJSON_Parse(text) : NULL;
  free(text);
  text = NULL;
  bool local = !strcmp(ny_author_string(signer, "keyId"), NY_AUTHOR_KEY_ID);
  cJSON_Delete(signer);
  if (ret < 0 || !local)
    return ret < 0 ? ret : -EACCES;
  *result = cJSON_CreateObject();
  if (!*result || !cJSON_AddStringToObject(*result, "id", id) ||
      !cJSON_AddStringToObject(*result, "version", config.version) ||
      !cJSON_AddStringToObject(*result, "name", config.name))
    return -ENOMEM;
  struct ny_scheduler_status_s status;
  ret = ny_scheduler_status(id, &status);
  cJSON_AddStringToObject(*result, "state",
                          ret == 0 ? status.state : "stopped");
  cJSON_AddNumberToObject(*result, "lastError",
                          ret == 0 ? status.last_error : 0);
  cJSON_AddStringToObject(*result, "errorText",
                          ret == 0 ? status.last_error_text : "");
  cJSON *permissions = cJSON_AddArrayToObject(*result, "permissions");
  for (uint64_t bit = 1; bit; bit <<= 1)
    if (config.requested_permissions & bit)
      cJSON_AddItemToArray(
          permissions, cJSON_CreateString(ny_manifest_permission_name(bit)));
  if (source)
    {
      ret = ny_author_join(file, sizeof(file), path, "source.js");
      if (ret == 0)
        ret = ny_author_read(file, NY_AUTHOR_SOURCE_MAX, &text);
      if (ret == 0 && !cJSON_AddStringToObject(*result, "source", text))
        ret = -ENOMEM;
      free(text);
    }
  else
    ret = 0;
  return ret;
}

/****************************************************************************
 * Name: ny_author_visit
 ****************************************************************************/

static int ny_author_visit(const char *id, const char *version,
                           const char *path, bool current, bool last_good,
                           void *opaque)
{
  (void)last_good;
  if (!current || !ny_author_id(id))
    return 0;
  struct ny_plugin_config_s config;
  int ret = ny_manifest_load(path, &config);
  if (ret < 0)
    return ret;
  char file[PATH_MAX], *text = NULL;
  ret = ny_author_join(file, sizeof(file), path, "signer.json");
  if (ret == 0)
    ret = ny_author_read(file, 1024, &text);
  cJSON *signer = text ? cJSON_Parse(text) : NULL;
  free(text);
  bool local = !strcmp(ny_author_string(signer, "keyId"), NY_AUTHOR_KEY_ID);
  cJSON_Delete(signer);
  if (!local)
    return 0;
  cJSON *item = cJSON_CreateObject();
  if (!item || !cJSON_AddStringToObject(item, "id", id) ||
      !cJSON_AddStringToObject(item, "version", version) ||
      !cJSON_AddStringToObject(item, "name", config.name) ||
      !cJSON_AddItemToArray(opaque, item))
    {
      cJSON_Delete(item);
      return -ENOMEM;
    }
  return 0;
}

/****************************************************************************
 * Name: ny_author_journal
 ****************************************************************************/

static int ny_author_journal(const cJSON *value)
{
  cJSON *old = NULL;
  uint64_t revision = 0;
  int ret =
      ny_product_store_read("plugin-author-transaction", &old, &revision);
  cJSON_Delete(old);
  return ret < 0 ? ret
                 : ny_product_store_write("plugin-author-transaction", value,
                                          revision, &revision);
}

/****************************************************************************
 * Name: ny_author_recover
 * Description: Restore an interrupted trial before any plugin operation.
 ****************************************************************************/

int ny_author_recover(void)
{
  cJSON *journal = NULL;
  uint64_t revision;
  int ret =
      ny_product_store_read("plugin-author-transaction", &journal, &revision);
  if (ret < 0 || !journal ||
      !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(journal, "pending")))
    {
      cJSON_Delete(journal);
      return ret;
    }
  const char *id = ny_author_string(journal, "id");
  if (!ny_author_id(id))
    {
      cJSON_Delete(journal);
      return -EBADMSG;
    }
  char storage[PATH_MAX] = "", prefix[PATH_MAX], backup[PATH_MAX];
  ret = ny_scheduler_stop(id);
  if (ret == -ENOENT)
    ret = 0;
  if (ret == 0)
    ret = ny_package_storage_root(id, storage, sizeof(storage));
  if (ret == 0)
    ret = ny_author_join(prefix, sizeof(prefix), storage, "data/");
  snprintf(backup, sizeof(backup), "author-backup/%s/", id);
  if (ret == 0)
    ret = ny_state_copy_prefix(backup, prefix);
  const char *version = ny_author_string(journal, "previousVersion");
  if (ret == 0 && version[0])
    {
      char current[PATH_MAX];
      struct ny_plugin_config_s config;
      ret = ny_package_resolve(id, current, sizeof(current));
      if (ret == 0)
        ret = ny_manifest_load(current, &config);
      if (ret == 0 && strcmp(config.version, version))
        ret = ny_package_rollback(id);
    }
  else if (ret == 0)
    ret = ny_package_deactivate(id);
  const cJSON *grants =
      cJSON_GetObjectItemCaseSensitive(journal, "previousGrants");
  if (ret == 0 && grants)
    {
      if (!cJSON_IsNumber(grants) || !isfinite(grants->valuedouble) ||
          grants->valuedouble < 0 ||
          grants->valuedouble > 9007199254740991.0 ||
          floor(grants->valuedouble) != grants->valuedouble)
        ret = -EBADMSG;
      else
        {
          uint64_t mask = (uint64_t)grants->valuedouble;
          for (uint64_t bit = 1; ret == 0 && bit; bit <<= 1)
            {
              const char *permission = ny_manifest_permission_name(bit);
              if (!permission)
                continue;
              ret = mask & bit ? ny_permission_grant(id, permission)
                               : ny_permission_revoke(id, permission);
              if (ret == -ENOENT)
                ret = 0;
            }
        }
    }
  if (ret == 0)
    {
      cJSON_DeleteItemFromObjectCaseSensitive(journal, "pending");
      if (!cJSON_AddBoolToObject(journal, "pending", false))
        ret = -ENOMEM;
      else
        ret = ny_author_journal(journal);
    }
  if (ret == 0 && version[0] &&
      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(journal, "wasRunning")))
    {
      char path[PATH_MAX];
      ret = ny_package_resolve(id, path, sizeof(path));
      if (ret == 0)
        ret = ny_scheduler_start_installed(path, storage);
    }
  cJSON_Delete(journal);
  return ret;
}

/****************************************************************************
 * Name: ny_author_deploy
 ****************************************************************************/

static int ny_author_deploy(const cJSON *data, cJSON **result)
{
  const char *id = ny_author_string(data, "id");
  const char *name = ny_author_string(data, "name");
  const char *source = ny_author_string(data, "source");
  const cJSON *base = cJSON_GetObjectItemCaseSensitive(data, "baseVersion");
  const cJSON *permissions =
      cJSON_GetObjectItemCaseSensitive(data, "permissions");
  if (strlen(source) > NY_AUTHOR_SOURCE_MAX)
    {
      *result = cJSON_CreateObject();
      if (*result != NULL)
        {
          cJSON_AddStringToObject(
              *result, "errorText",
              "Source exceeds 8192 UTF-8 bytes; shorten the complete module.");
          cJSON_AddNumberToObject(*result, "sourceBytes", strlen(source));
          cJSON_AddNumberToObject(*result, "sourceLimit",
                                  NY_AUTHOR_SOURCE_MAX);
        }

      return -E2BIG;
    }

  if (!ny_author_id(id) || !name[0] || strlen(name) >= NY_PLUGIN_NAME_SIZE ||
      !source[0] || !cJSON_IsString(base) || !cJSON_IsArray(permissions) ||
      cJSON_GetArraySize(permissions) > 16)
    return -EINVAL;
  bool has_ui = false, requires_nas = false;
  const cJSON *permission;
  cJSON_ArrayForEach(permission, permissions)
  {
    bool allowed = false;
    for (size_t i = 0;
         cJSON_IsString(permission) &&
         i < sizeof(g_author_permissions) / sizeof(g_author_permissions[0]);
         i++)
      allowed |= !strcmp(permission->valuestring, g_author_permissions[i]);
    for (size_t i = 0; cJSON_IsString(permission) &&
                       i < sizeof(g_author_nas_permissions) /
                               sizeof(g_author_nas_permissions[0]);
         i++)
      if (!strcmp(permission->valuestring, g_author_nas_permissions[i]))
        {
          allowed = true;
          requires_nas = true;
        }
    if (!allowed)
      return -EACCES;
    has_ui |= !strcmp(permission->valuestring, "ui.page") ||
              !strcmp(permission->valuestring, "ui.html");
  }
  if (!has_ui)
    return -EINVAL;
  cJSON *previous = NULL;
  int ret = ny_author_inspect(id, false, &previous);
  if (ret != 0 && ret != -ENOENT)
    {
      cJSON_Delete(previous);
      return ret;
    }
  const char *old_version = ny_author_string(previous, "version");
  if (strcmp(base->valuestring, old_version))
    {
      cJSON_Delete(previous);
      return -ESTALE;
    }
  cJSON *policy = NULL;
  uint64_t revision = 0;
  ret = ny_author_policy(&policy, &revision);
  if (ret == 0 &&
      !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(policy, "enabled")))
    ret = -EACCES;
  if (ret == 0 && requires_nas &&
      !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(policy, "nas")))
    ret = -EACCES;
  if (ret == 0)
    ret = ny_product_store_write("plugin-author", policy, revision, &revision);
  cJSON_Delete(policy);
  if (ret < 0)
    {
      cJSON_Delete(previous);
      return ret;
    }
  char version[NY_PLUGIN_VERSION_SIZE];
  snprintf(version, sizeof(version), "1.0.%" PRIu64, revision);
  char root[PATH_MAX];
  snprintf(root, sizeof(root), "%s/%s-%s", NY_AUTHOR_ROOT, id, version);
  if (ret == 0)
    ret = ny_author_directory(root);
  cJSON *manifest = cJSON_CreateObject();
  cJSON *requested = cJSON_Duplicate(permissions, true);
  bool valid = manifest && requested &&
               cJSON_AddStringToObject(manifest, "id", id) &&
               cJSON_AddStringToObject(manifest, "name", name) &&
               cJSON_AddStringToObject(manifest, "version", version) &&
               cJSON_AddStringToObject(manifest, "runtime", "quickjs") &&
               cJSON_AddStringToObject(manifest, "entry", "main.js") &&
               cJSON_AddNumberToObject(manifest, "apiVersion", 1) &&
               cJSON_AddBoolToObject(manifest, "background", false);
  if (!valid || !cJSON_AddItemToObject(manifest, "permissions", requested))
    {
      cJSON_Delete(requested);
      valid = false;
    }
  char *encoded = valid ? cJSON_PrintUnformatted(manifest) : NULL;
  if (ret == 0)
    ret = encoded ? ny_author_sign(root, encoded, source) : -ENOMEM;
  free(encoded);
  cJSON_Delete(manifest);
  if (ret == 0)
    ret = ny_package_install(root);
  char storage[PATH_MAX] = "", prefix[PATH_MAX], backup[PATH_MAX];
  if (ret == 0)
    ret = ny_package_storage_root(id, storage, sizeof(storage));
  struct ny_plugin_config_s previous_permissions = { 0 };
  snprintf(previous_permissions.id, sizeof(previous_permissions.id), "%s", id);
  previous_permissions.requested_permissions = UINT64_MAX;
  if (ret == 0)
    ret = ny_permission_apply(&previous_permissions);
  cJSON *journal = cJSON_CreateObject();
  bool journal_valid =
      journal && cJSON_AddStringToObject(journal, "id", id) &&
      cJSON_AddStringToObject(journal, "previousVersion", old_version) &&
      cJSON_AddNumberToObject(journal, "previousGrants",
                              previous_permissions.permissions) &&
      cJSON_AddBoolToObject(
          journal, "wasRunning",
          !strcmp(ny_author_string(previous, "state"), "running")) &&
      cJSON_AddBoolToObject(journal, "pending", true);
  if (ret == 0 && !journal_valid)
    ret = -ENOMEM;
  bool stopped = false, pending = false;
  if (ret == 0)
    {
      ret = ny_scheduler_stop(id);
      if (ret == -ENOENT)
        ret = 0;
      stopped = ret == 0;
    }
  if (ret == 0)
    ret = ny_author_join(prefix, sizeof(prefix), storage, "data/");
  snprintf(backup, sizeof(backup), "author-backup/%s/", id);
  if (ret == 0)
    ret = ny_state_copy_prefix(prefix, backup);
  if (ret == 0)
    {
      ret = ny_author_journal(journal);
      pending = ret == 0;
    }
  cJSON_ArrayForEach(permission, permissions) if (ret == 0) ret =
      ny_permission_grant(id, permission->valuestring);
  char installed[PATH_MAX];
  char diagnostic[256] = "";
  if (ret == 0)
    ret =
        ny_package_resolve_version(id, version, installed, sizeof(installed));
  if (ret == 0)
    ret = ny_scheduler_start_installed_report(
        installed, storage,
        ~(NY_PERMISSION_NAS_WRITE | NY_PERMISSION_NAS_MANAGE), diagnostic,
        sizeof(diagnostic));
  cJSON *ui = NULL;
  if (ret == 0)
    ret = ny_ui_tree(id, &ui);
  cJSON_Delete(ui);
  if (ret == 0)
    ret = ny_package_activate(id, version);
  if (ret == 0)
    ret = ny_scheduler_refresh_permissions(id);
  if (ret == 0)
    {
      cJSON_DeleteItemFromObjectCaseSensitive(journal, "pending");
      ret = cJSON_AddBoolToObject(journal, "pending", false)
                ? ny_author_journal(journal)
                : -ENOMEM;
    }
  if (ret < 0 && pending)
    {
      int recovery = ny_author_recover();
      if (recovery < 0)
        ret = recovery;
    }
  else if (ret < 0 && stopped && old_version[0] &&
           !strcmp(ny_author_string(previous, "state"), "running"))
    {
      char old_path[PATH_MAX];
      if (ny_package_resolve(id, old_path, sizeof(old_path)) == 0)
        ny_scheduler_start_installed(old_path, storage);
    }
  if (ret == 0)
    {
      ny_ui_changed();
      ret = ny_author_inspect(id, false, result);
      if (ret == 0)
        cJSON_AddBoolToObject(*result, "startupVerified", true);
    }
  else
    {
      *result = cJSON_CreateObject();
      if (*result)
        {
          cJSON_AddBoolToObject(*result, "ok", false);
          cJSON_AddNumberToObject(*result, "error", ret);
          cJSON_AddStringToObject(*result, "errorText", diagnostic);
          cJSON_AddStringToObject(*result, "previousVersion", old_version);
        }
    }
  cJSON_Delete(journal);
  cJSON_Delete(previous);
  ny_author_cleanup(root);
  return ret;
}

/****************************************************************************
 * Name: ny_author_inputs
 ****************************************************************************/

static int ny_author_inputs(const cJSON *node, cJSON *values, cJSON *fields,
                            const cJSON *overrides)
{
  static const char *const kinds[] = {
    "slider",      "switch",    "chips",     "textField",
    "colorPicker", "toggleRow", "sliderRow", "segmentRow",
    "chipSelect",  "dial",      "timeWheel", "colorSwatch"
  };
  const char *id = ny_author_string(node, "id");
  const char *type = ny_author_string(node, "type");
  const cJSON *child;

  for (size_t i = 0; id[0] && i < sizeof(kinds) / sizeof(kinds[0]); i++)
    {
      if (strcmp(type, kinds[i]) == 0)
        {
          if (!cJSON_AddBoolToObject(fields, id, true))
            return -ENOMEM;
          const cJSON *value = cJSON_GetObjectItemCaseSensitive(overrides, id);
          if (value == NULL)
            value = cJSON_GetObjectItemCaseSensitive(
                cJSON_GetObjectItemCaseSensitive(node, "props"), "value");
          if (value == NULL)
            break;
          cJSON *copy = cJSON_Duplicate(value, true);
          if (copy == NULL || !cJSON_AddItemToObject(values, id, copy))
            {
              cJSON_Delete(copy);
              return -ENOMEM;
            }
          break;
        }
    }

  cJSON_ArrayForEach(child, cJSON_GetObjectItemCaseSensitive(node, "children"))
  {
    int ret = ny_author_inputs(child, values, fields, overrides);
    if (ret < 0)
      return ret;
  }
  return 0;
}

/****************************************************************************
 * Name: ny_author_test_event
 ****************************************************************************/

static int ny_author_test_event(const char *id, const cJSON *event,
                                char *diagnostic, size_t capacity)
{
  cJSON *tree = NULL;
  int ret = ny_ui_tree(id, &tree);
  if (ret < 0)
    return ret;
  const cJSON *document = tree;
  const cJSON *views = cJSON_GetObjectItemCaseSensitive(tree, "views");
  if (views != NULL)
    {
      const char *view = ny_author_string(event, "view");
      if (!view[0])
        view = cJSON_GetObjectItemCaseSensitive(views, "component")
                   ? "component"
                   : "html";
      document = cJSON_GetObjectItemCaseSensitive(views, view);
    }
  cJSON *prepared = cJSON_Duplicate(event, true);
  cJSON *values = cJSON_CreateObject();
  cJSON *fields = cJSON_CreateObject();
  if (!document || !prepared || !values || !fields)
    ret = !document ? -EINVAL : -ENOMEM;
  if (ret == 0 && !cJSON_GetObjectItemCaseSensitive(document, "html"))
    {
      const cJSON *overrides =
          cJSON_GetObjectItemCaseSensitive(event, "values");
      if (overrides && !cJSON_IsObject(overrides))
        ret = -EINVAL;
      else
        ret = ny_author_inputs(
            cJSON_GetObjectItemCaseSensitive(document, "root"), values, fields,
            overrides);
      const cJSON *item;
      cJSON_ArrayForEach(
          item, overrides) if (ret == 0 && (!item->string ||
                                            !cJSON_GetObjectItemCaseSensitive(
                                                fields, item->string)))
      {
        snprintf(diagnostic, capacity, "Unknown input field: %s",
                 item->string ? item->string : "(unnamed)");
        ret = -EINVAL;
      }
      if (ret == 0)
        {
          cJSON_DeleteItemFromObjectCaseSensitive(prepared, "values");
          if (cJSON_AddItemToObject(prepared, "values", values))
            values = NULL;
          else
            ret = -ENOMEM;
        }
    }
  if (ret == 0)
    ret = ny_ui_event(id, prepared);
  cJSON_Delete(values);
  cJSON_Delete(fields);
  cJSON_Delete(prepared);
  cJSON_Delete(tree);
  return ret;
}

/****************************************************************************
 * Name: ny_author_test
 ****************************************************************************/

static int ny_author_test(const cJSON *data, cJSON **result)
{
  const char *id = ny_author_string(data, "id");
  const cJSON *event = cJSON_GetObjectItemCaseSensitive(data, "event");
  struct ny_scheduler_status_s initial;
  struct ny_scheduler_status_s status;
  uint64_t deadline;
  char diagnostic[256] = "";
  int ret;

  if (!ny_author_id(id) || !cJSON_IsObject(event))
    {
      return -EINVAL;
    }

  ret = ny_scheduler_status(id, &initial);
  if (ret < 0)
    {
      return ret;
    }

  status = initial;
  ret = ny_author_test_event(id, event, diagnostic, sizeof(diagnostic));
  deadline = ny_product_time_ms(true) + NY_AUTHOR_TEST_TIMEOUT_MS;
  while (ret == 0)
    {
      ret = ny_scheduler_status(id, &status);
      if (ret < 0)
        {
          break;
        }

      if (status.generation != initial.generation)
        {
          ret = -ESTALE;
          break;
        }

      if (strcmp(status.state, "running") != 0)
        {
          ret = status.last_error < 0 ? status.last_error : -EPIPE;
          break;
        }

      if (status.queued == 0 && !status.dispatching)
        {
          break;
        }

      if (ny_product_time_ms(true) >= deadline)
        {
          ret = -ETIMEDOUT;
          break;
        }

      usleep(10000);
    }

  *result = cJSON_CreateObject();
  if (*result == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddBoolToObject(*result, "completed", ret == 0);
  cJSON_AddStringToObject(*result, "state", status.state);
  cJSON_AddNumberToObject(*result, "error", ret);
  cJSON_AddStringToObject(*result, "errorText",
                          diagnostic[0] ? diagnostic : status.last_error_text);
  return ret;
}

/****************************************************************************
 * Name: ny_author_request
 ****************************************************************************/

int ny_author_request(const struct ny_product_caller_s *caller,
                      const char *topic, const cJSON *data, cJSON **result)
{
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  const char *op = topic + strlen("plugin.author.");
  if (!strcmp(op, "list"))
    {
      *result = cJSON_CreateObject();
      cJSON *plugins = cJSON_AddArrayToObject(*result, "plugins");
      return !*result || !plugins ? -ENOMEM
                                  : ny_package_visit(ny_author_visit, plugins);
    }
  if (!strcmp(op, "contract") || !strcmp(op, "policy.get") ||
      !strcmp(op, "policy.set"))
    {
      cJSON *policy = NULL;
      uint64_t revision;
      int ret = ny_author_policy(&policy, &revision);
      if (ret == 0 && !strcmp(op, "policy.set"))
        {
          const cJSON *enabled =
              cJSON_GetObjectItemCaseSensitive(data, "enabled");
          const cJSON *nas = cJSON_GetObjectItemCaseSensitive(data, "nas");
          if (!caller->local_transport || !cJSON_IsBool(enabled))
            ret = -EACCES;
          else if (nas && !cJSON_IsBool(nas))
            ret = -EINVAL;
#ifndef CONFIG_NYABULA_CORE_NAS
          else if (cJSON_IsTrue(nas))
            ret = -ENOTSUP;
#endif
          else
            {
              if (nas)
                {
                  cJSON_DeleteItemFromObjectCaseSensitive(policy, "nas");
                  if (!cJSON_AddBoolToObject(policy, "nas", cJSON_IsTrue(nas)))
                    ret = -ENOMEM;
                }
              cJSON_DeleteItemFromObjectCaseSensitive(policy, "enabled");
              ret = ret < 0 ? ret
                    : cJSON_AddBoolToObject(policy, "enabled",
                                            cJSON_IsTrue(enabled))
                        ? ny_product_store_write("plugin-author", policy,
                                                 revision, &revision)
                        : -ENOMEM;
            }
        }
      if (ret < 0)
        {
          cJSON_Delete(policy);
          return ret;
        }
      cJSON_AddNumberToObject(policy, "sourceLimit", NY_AUTHOR_SOURCE_MAX);
      cJSON_AddNumberToObject(policy, "storageValueLimit",
                              CONFIG_NYABULA_CORE_STORAGE_VALUE_LIMIT);
      cJSON_AddNumberToObject(policy, "storageTotalLimit",
                              CONFIG_NYABULA_CORE_STORAGE_BYTES_LIMIT);
#ifdef CONFIG_NYABULA_CORE_NAS
      cJSON_AddBoolToObject(policy, "nasSupported", true);
#else
      cJSON_AddBoolToObject(policy, "nasSupported", false);
#endif
      cJSON *permissions = cJSON_AddArrayToObject(policy, "permissions");
      for (size_t i = 0;
           i < sizeof(g_author_permissions) / sizeof(g_author_permissions[0]);
           i++)
        cJSON_AddItemToArray(permissions,
                             cJSON_CreateString(g_author_permissions[i]));
      if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(policy, "nas")))
        for (size_t i = 0; i < sizeof(g_author_nas_permissions) /
                                   sizeof(g_author_nas_permissions[0]);
             i++)
          cJSON_AddItemToArray(
              permissions, cJSON_CreateString(g_author_nas_permissions[i]));
      if (!strcmp(op, "contract"))
        cJSON_AddStringToObject(policy, "guide", g_author_contract);
      *result = policy;
      return 0;
    }
  if (!strcmp(op, "inspect"))
    return ny_author_inspect(ny_author_string(data, "id"), true, result);
  if (!strcmp(op, "deploy"))
    return ny_author_deploy(data, result);
  if (!strcmp(op, "tree"))
    {
      const char *id = ny_author_string(data, "id");
      return ny_author_id(id) ? ny_ui_tree(id, result) : -EINVAL;
    }
  if (!strcmp(op, "test"))
    return ny_author_test(data, result);
  return -ENOSYS;
}
