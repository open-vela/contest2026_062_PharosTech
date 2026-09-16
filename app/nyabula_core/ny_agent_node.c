/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_node.c
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

#include "agent_config.h"
#include "infra/config_store.h"
#include "node/node_client.h"
#include "ny_agent.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <nuttx/mutex.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_AI_AGENT_NODE
static mutex_t g_node_lock = NXMUTEX_INITIALIZER;
static bool g_node_requested;
static bool g_node_running;
static int g_node_error;
static const char *const g_node_commands[] = {
  "nyabula.status",       "nyabula.read",   "nyabula.chat",
  "nyabula.run.get",      "nyabula.cancel", "nyabula.expression",
  "nyabula.tools.catalog"
};

static const char *ny_agent_node_string(const cJSON *data, const char *key);
static int ny_agent_node_identity(bool create, unsigned char *public_key,
                                  unsigned char *secret_key, char *id);
static cJSON *ny_agent_node_commands(void);
static char *ny_agent_node_connect(const char *challenge, const char *token);
static void ny_agent_node_observe(const char *response);
static int ny_agent_node_execute(const char *command, const char *input,
                                 char *output, size_t capacity);
static cJSON *ny_agent_node_status(void);

/****************************************************************************
 * Name: ny_agent_node_string
 ****************************************************************************/

static const char *ny_agent_node_string(const cJSON *data, const char *key)
{
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(data, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

/****************************************************************************
 * Name: ny_agent_node_identity
 ****************************************************************************/

static int ny_agent_node_identity(bool create, unsigned char *public_key,
                                  unsigned char *secret_key, char *id)
{
  char encoded[65] = { 0 };
  unsigned char seed[crypto_sign_SEEDBYTES];
  claw_config_get("node_device_seed", encoded, sizeof(encoded));
  if (!encoded[0])
    {
      if (!create)
        return -ENOENT;
      if (agent_secure_random(seed, sizeof(seed)) != 0)
        return -EIO;
      sodium_bin2hex(encoded, sizeof(encoded), seed, sizeof(seed));
      if (claw_config_set("node_device_seed", encoded) != 0)
        {
          sodium_memzero(seed, sizeof(seed));
          return -EIO;
        }
    }
  size_t length;
  int ret = sodium_hex2bin(seed, sizeof(seed), encoded, strlen(encoded), NULL,
                           &length, NULL);
  sodium_memzero(encoded, sizeof(encoded));
  if (ret != 0 || length != sizeof(seed))
    return -EBADMSG;
  crypto_sign_seed_keypair(public_key, secret_key, seed);
  sodium_memzero(seed, sizeof(seed));
  unsigned char hash[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(hash, public_key, crypto_sign_PUBLICKEYBYTES);
  sodium_bin2hex(id, 65, hash, sizeof(hash));
  return 0;
}

/****************************************************************************
 * Name: ny_agent_node_commands
 ****************************************************************************/

static cJSON *ny_agent_node_commands(void)
{
  cJSON *commands = cJSON_CreateArray();
  if (commands == NULL)
    return NULL;
  for (size_t i = 0; i < sizeof(g_node_commands) / sizeof(g_node_commands[0]);
       i++)
    {
      cJSON *name = cJSON_CreateString(g_node_commands[i]);
      if (name == NULL || !cJSON_AddItemToArray(commands, name))
        {
          cJSON_Delete(name);
          cJSON_Delete(commands);
          return NULL;
        }
    }
  return commands;
}

/****************************************************************************
 * Name: ny_agent_node_connect
 ****************************************************************************/

static char *ny_agent_node_connect(const char *challenge, const char *token)
{
  cJSON *input = cJSON_Parse(challenge);
  const char *nonce = ny_agent_node_string(input, "nonce");
  const cJSON *stamp = cJSON_GetObjectItemCaseSensitive(input, "ts");
  if (!nonce[0] || strlen(nonce) > 255 || !cJSON_IsNumber(stamp) ||
      !isfinite(stamp->valuedouble) || stamp->valuedouble < 0 ||
      stamp->valuedouble > 9007199254740991.0 ||
      floor(stamp->valuedouble) != stamp->valuedouble)
    {
      cJSON_Delete(input);
      return NULL;
    }
  unsigned char public_key[crypto_sign_PUBLICKEYBYTES];
  unsigned char secret_key[crypto_sign_SECRETKEYBYTES];
  char device_id[65];
  if (ny_agent_node_identity(true, public_key, secret_key, device_id) < 0)
    {
      cJSON_Delete(input);
      return NULL;
    }
  char payload[1024];
  int count =
      snprintf(payload, sizeof(payload),
               "v3|%s|node-host|node|node||%" PRIu64 "|%s|%s|openvela|robot",
               device_id, (uint64_t)stamp->valuedouble, token, nonce);
  unsigned char signature[crypto_sign_BYTES];
  int ret = count < 0 || (size_t)count >= sizeof(payload)
                ? -E2BIG
                : crypto_sign_detached(signature, NULL,
                                       (const unsigned char *)payload, count,
                                       secret_key);
  sodium_memzero(secret_key, sizeof(secret_key));
  sodium_memzero(payload, sizeof(payload));
  if (ret < 0)
    {
      cJSON_Delete(input);
      return NULL;
    }
  char public_text[64], signature_text[96];
  sodium_bin2base64(public_text, sizeof(public_text), public_key,
                    sizeof(public_key),
                    sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  sodium_bin2base64(signature_text, sizeof(signature_text), signature,
                    sizeof(signature),
                    sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  cJSON *params = cJSON_CreateObject();
  cJSON *client = cJSON_AddObjectToObject(params, "client");
  cJSON *device = cJSON_AddObjectToObject(params, "device");
  cJSON *auth = cJSON_AddObjectToObject(params, "auth");
  bool valid =
      params != NULL && client != NULL && device != NULL && auth != NULL;
  valid &= cJSON_AddNumberToObject(params, "minProtocol", 3) != NULL;
  valid &= cJSON_AddNumberToObject(params, "maxProtocol", 4) != NULL;
  valid &= cJSON_AddStringToObject(client, "id", "node-host") != NULL;
  valid &= cJSON_AddStringToObject(client, "displayName", "Nyabula") != NULL;
  valid &= cJSON_AddStringToObject(client, "version", "1.0.0") != NULL;
  valid &= cJSON_AddStringToObject(client, "platform", "openvela") != NULL;
  valid &= cJSON_AddStringToObject(client, "deviceFamily", "robot") != NULL;
  valid &= cJSON_AddStringToObject(client, "mode", "node") != NULL;
  valid &= cJSON_AddStringToObject(params, "role", "node") != NULL;
  valid &= cJSON_AddArrayToObject(params, "scopes") != NULL;
  valid &= cJSON_AddArrayToObject(params, "caps") != NULL;
  valid &= cJSON_AddObjectToObject(params, "permissions") != NULL;
  cJSON *commands = ny_agent_node_commands();
  if (commands == NULL || !cJSON_AddItemToObject(params, "commands", commands))
    {
      cJSON_Delete(commands);
      valid = false;
    }
  valid &= cJSON_AddStringToObject(auth, "token", token) != NULL;
  valid &= cJSON_AddStringToObject(device, "id", device_id) != NULL;
  valid &= cJSON_AddStringToObject(device, "publicKey", public_text) != NULL;
  valid &=
      cJSON_AddStringToObject(device, "signature", signature_text) != NULL;
  valid &=
      cJSON_AddNumberToObject(device, "signedAt", stamp->valuedouble) != NULL;
  valid &= cJSON_AddStringToObject(device, "nonce", nonce) != NULL;
  char *encoded = valid ? cJSON_PrintUnformatted(params) : NULL;
  cJSON_Delete(params);
  cJSON_Delete(input);
  return encoded;
}

/****************************************************************************
 * Name: ny_agent_node_observe
 ****************************************************************************/

static void ny_agent_node_observe(const char *response)
{
  cJSON *root = cJSON_Parse(response);
  const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
  const cJSON *auth = cJSON_GetObjectItemCaseSensitive(payload, "auth");
  const char *token = ny_agent_node_string(auth, "deviceToken");
  if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok")) && token[0] &&
      strlen(token) <= 255)
    claw_config_set("gateway_device_token", token);
  cJSON_Delete(root);
}

/****************************************************************************
 * Name: ny_agent_node_execute
 ****************************************************************************/

static int ny_agent_node_execute(const char *command, const char *input,
                                 char *output, size_t capacity)
{
  const char *topic =
      !strcmp(command, "nyabula.status")          ? "agent.status"
      : !strcmp(command, "nyabula.chat")          ? "agent.chat"
      : !strcmp(command, "nyabula.run.get")       ? "agent.run.get"
      : !strcmp(command, "nyabula.cancel")        ? "agent.cancel"
      : !strcmp(command, "nyabula.expression")    ? "eyes.expression"
      : !strcmp(command, "nyabula.tools.catalog") ? "agent.tools.catalog"
                                                  : NULL;
  cJSON *data = cJSON_Parse(input);
  cJSON *empty = cJSON_CreateObject();
  if (!strcmp(command, "nyabula.read"))
    {
      static const char *const reads[] = { "sys.info",      "system.time.get",
                                           "memory.list",   "task.list",
                                           "calendar.list", "timer.list",
                                           "eyes.status",   "device.status" };
      const char *requested = ny_agent_node_string(data, "topic");
      for (size_t i = 0; i < sizeof(reads) / sizeof(reads[0]); i++)
        if (!strcmp(requested, reads[i]))
          topic = reads[i];
    }
  struct ny_product_caller_s caller = { .id = "openclaw",
                                        .role = NY_PRODUCT_OWNER,
                                        .local_transport = false };
  cJSON *result = NULL;
  int ret =
      !topic || !cJSON_IsObject(data) || empty == NULL
          ? -EINVAL
          : ny_product_request(&caller, topic,
                               !strcmp(command, "nyabula.read") ? empty : data,
                               &result);
  if (ret == 0 && !cJSON_PrintPreallocated(result, output, capacity, false))
    ret = -E2BIG;
  if (ret < 0)
    snprintf(output, capacity, "Core command failed (%d).", ret);
  cJSON_Delete(result);
  cJSON_Delete(data);
  cJSON_Delete(empty);
  return ret;
}

/****************************************************************************
 * Name: ny_agent_node_status
 ****************************************************************************/

static cJSON *ny_agent_node_status(void)
{
  char host[128] = { 0 }, port[8] = { 0 }, token[256] = { 0 },
       device_token[256] = { 0 }, tls[8] = { 0 };
  claw_config_get(AGENT_CFG_KEY_GATEWAY_HOST, host, sizeof(host));
  claw_config_get(AGENT_CFG_KEY_GATEWAY_PORT, port, sizeof(port));
  claw_config_get(AGENT_CFG_KEY_GATEWAY_TOKEN, token, sizeof(token));
  claw_config_get("gateway_device_token", device_token, sizeof(device_token));
  claw_config_get("gateway_tls", tls, sizeof(tls));
  char id[65] = { 0 }, error[128] = { 0 };
  unsigned char public_key[crypto_sign_PUBLICKEYBYTES],
      secret_key[crypto_sign_SECRETKEYBYTES];
  ny_agent_node_identity(false, public_key, secret_key, id);
  sodium_memzero(secret_key, sizeof(secret_key));
  int protocol = 0;
  int stage = node_client_status(error, sizeof(error), &protocol);
  cJSON *result = cJSON_CreateObject();
  if (result == NULL)
    return NULL;
  cJSON_AddStringToObject(result, "host", host);
  cJSON_AddStringToObject(result, "port", port[0] ? port : "18789");
  cJSON_AddBoolToObject(result, "tls",
                        tls[0] ? !strcmp(tls, "1") : !strcmp(port, "443"));
  cJSON_AddBoolToObject(result, "tokenSet", token[0] != 0);
  cJSON_AddBoolToObject(result, "deviceTokenSet", device_token[0] != 0);
  cJSON_AddStringToObject(result, "deviceId", id);
  cJSON_AddBoolToObject(result, "requested", g_node_requested);
  cJSON_AddBoolToObject(result, "running", g_node_running);
  cJSON_AddBoolToObject(result, "connected", stage >= 1);
  cJSON_AddBoolToObject(result, "ready", stage == 2);
  cJSON_AddNumberToObject(result, "protocol", protocol);
  cJSON_AddNumberToObject(result, "lastError", g_node_error);
  cJSON_AddStringToObject(result, "gatewayError", error);
  cJSON_AddItemToObject(result, "commands", ny_agent_node_commands());
  return result;
}
#endif

/****************************************************************************
 * Name: ny_agent_node
 ****************************************************************************/

int ny_agent_node(const char *topic, const cJSON *data, cJSON **result)
{
#ifndef CONFIG_AI_AGENT_NODE
  (void)topic;
  (void)data;
  (void)result;
  return -ENOSYS;
#else
  int ret = 0;
  nxmutex_lock(&g_node_lock);
  if (!strcmp(topic, "agent.node.get"))
    {
    }
  else if (!strcmp(topic, "agent.node.stop"))
    {
      g_node_requested = false;
      node_client_request_stop();
    }
  else if (g_node_running || g_node_requested)
    ret = -EBUSY;
  else if (!strcmp(topic, "agent.node.save"))
    {
      const char *host = ny_agent_node_string(data, "host");
      const char *port = ny_agent_node_string(data, "port");
      const cJSON *key = cJSON_GetObjectItemCaseSensitive(data, "token");
      const cJSON *tls = cJSON_GetObjectItemCaseSensitive(data, "tls");
      char old_host[128] = { 0 }, old_port[8] = { 0 }, token[256] = { 0 },
           cached[256] = { 0 };
      claw_config_get(AGENT_CFG_KEY_GATEWAY_HOST, old_host, sizeof(old_host));
      claw_config_get(AGENT_CFG_KEY_GATEWAY_PORT, old_port, sizeof(old_port));
      claw_config_get(AGENT_CFG_KEY_GATEWAY_TOKEN, token, sizeof(token));
      claw_config_get("gateway_device_token", cached, sizeof(cached));
      if (!host[0] || strlen(host) > 127 || !port[0] || strlen(port) > 5 ||
          strspn(port, "0123456789") != strlen(port) || atoi(port) < 1 ||
          atoi(port) > 65535 || !cJSON_IsBool(tls) ||
          (key != NULL &&
           (!cJSON_IsString(key) || strlen(key->valuestring) > 255)))
        ret = -EINVAL;
      else
        {
          const char *keys[] = { AGENT_CFG_KEY_GATEWAY_HOST,
                                 AGENT_CFG_KEY_GATEWAY_PORT,
                                 AGENT_CFG_KEY_GATEWAY_TOKEN, "gateway_tls",
                                 "gateway_device_token" };
          const char *values[] = {
            host, port, key ? key->valuestring : token,
            cJSON_IsTrue(tls) ? "1" : "0",
            !strcmp(host, old_host) && !strcmp(port, old_port) ? cached : ""
          };
          ret = claw_config_set_many(keys, values, 5) == 0 ? 0 : -EIO;
        }
    }
  else if (!strcmp(topic, "agent.node.start"))
    {
      char host[128] = { 0 };
      claw_config_get(AGENT_CFG_KEY_GATEWAY_HOST, host, sizeof(host));
      if (!host[0])
        ret = -ENODATA;
      else
        g_node_requested = true;
    }
  else
    ret = -ENOSYS;
  if (strcmp(topic, "agent.node.get") || ret < 0)
    g_node_error = ret;
  if (ret == 0)
    {
      *result = ny_agent_node_status();
      if (*result == NULL)
        ret = -ENOMEM;
    }
  nxmutex_unlock(&g_node_lock);
  return ret;
#endif
}

/****************************************************************************
 * Name: ny_agent_node_tick
 ****************************************************************************/

void ny_agent_node_tick(void)
{
#ifdef CONFIG_AI_AGENT_NODE
  nxmutex_lock(&g_node_lock);
  if (g_node_requested && !g_node_running)
    {
      node_client_set_adapter(ny_agent_node_connect, ny_agent_node_execute,
                              ny_agent_node_observe);
      g_node_error = node_client_init();
      if (g_node_error == 0)
        g_node_error = node_client_start();
      g_node_running = g_node_error == 0;
      if (!g_node_running)
        g_node_requested = false;
    }
  if (g_node_running && node_client_exited())
    {
      node_client_stop();
      if (g_node_requested)
        g_node_error = -EIO;
      g_node_requested = false;
      g_node_running = false;
    }
  nxmutex_unlock(&g_node_lock);
#endif
}
