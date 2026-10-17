/****************************************************************************
 * app/nyabula_core/ny_web.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* Explicitly started, bounded local endpoint. An exact
 * Origin and a provisioned token are mandatory. Use a trusted tunnel for
 * non-loopback access: this endpoint does not implement TLS or pairing.
 */

#include "ny_web.h"
#include "ny_product.h"
#include "ny_web_auth.h"
#include "ny_websocket.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/tcp.h>
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <nyabula_eye_service.h>
#include <nyabula_eye_wire.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define NYABULA_WS_TOKEN_SIZE       64
#define NYABULA_WS_SOURCE           "webui"
#define NYABULA_WS_PRIORITY         40
#define NYABULA_WS_LEASE_MS         5000
#define NYABULA_WS_STATE_MS         50
#define NYABULA_WS_IDLE_MS          20000
#define NYABULA_WS_AUTH_MS          30000
#define NYABULA_WS_MAX_REQUESTS     40

#define NY_WEB_SEND_TIMEOUT_S       5
#define NY_WEB_KEEPALIVE_IDLE_S     10
#define NY_WEB_KEEPALIVE_INTERVAL_S 5
#define NY_WEB_KEEPALIVE_PROBES     3
#define NY_WEB_MAX_CLIENTS          8
#define NY_WEB_STORE_WAIT_MS        15000

static mutex_t g_web_lock = NXMUTEX_INITIALIZER;
static bool g_web_running;
static bool g_web_stopping;
static int g_web_clients[NY_WEB_MAX_CLIENTS];

/* Held only while the product service runs, for the one other party that
 * legitimately needs it: the code that draws the pairing QR on the eyes.
 */

static char g_web_product_token[NYABULA_WS_TOKEN_SIZE + 1];

/* Sockets that have said hello, i.e. panels someone has open. */

static int g_web_panels;

struct ny_web_client_args_s
{
  int slot;
  int fd;
  char token[NYABULA_WS_TOKEN_SIZE + 1];
  char origin[256];
};

struct nyabula_eye_ws_session_s
{
  char message[NYABULA_WS_MESSAGE_MAX + 1];
  unsigned char frame[NYABULA_WS_MESSAGE_MAX];
  char output[NYABULA_WS_MESSAGE_MAX + 1];
  bool authenticated;
  bool pair_auth;
  bool fragmented;
  size_t used;
  uint64_t fragment_deadline;
  uint64_t revision;
  bool published;
  uint64_t window;
  unsigned int requests;
};

static const char *nyabula_eye_ws_string(cJSON *json, const char *key);
static int nyabula_eye_ws_emit(int fd, struct nyabula_eye_ws_session_s *s,
                               const char *type, const char *id,
                               const char *topic, cJSON *data);
static int nyabula_eye_ws_error(int fd, struct nyabula_eye_ws_session_s *s,
                                const char *id, const char *topic,
                                const char *code);
static bool nyabula_eye_ws_auth(const char *actual, const char *expected);
static int nyabula_eye_ws_request(int fd, struct nyabula_eye_ws_session_s *s,
                                  const char *token);
static int nyabula_eye_ws_client(int fd, const char *token,
                                 const char *origin);
static void *ny_web_client_worker(void *argument);
static void ny_web_client_limits(int fd);
static void ny_web_store_wait(const char *path);
static int ny_web_token_load(const char *path, char *token);
static int ny_web_token_create(const char *path, char *token);
#ifdef CONFIG_NYABULA_CORE_PRODUCT
static const char *ny_web_error_name(int status);
#endif

/****************************************************************************
 * Name: nyabula_eye_ws_string
 ****************************************************************************/

static const char *nyabula_eye_ws_string(cJSON *json, const char *key)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
  return cJSON_IsString(item) ? item->valuestring : NULL;
}

/****************************************************************************
 * Name: nyabula_eye_ws_emit
 ****************************************************************************/

static int nyabula_eye_ws_emit(int fd, struct nyabula_eye_ws_session_s *s,
                               const char *type, const char *id,
                               const char *topic, cJSON *data)
{
  cJSON *root = cJSON_CreateObject();
  int ret = -ENOMEM;
  if (root == NULL || data == NULL)
    {
      cJSON_Delete(root);
      cJSON_Delete(data);
      return -ENOMEM;
    }

  cJSON_AddNumberToObject(root, "v", 1);
  cJSON_AddStringToObject(root, "type", type);
  cJSON_AddStringToObject(root, "topic", topic);
  if (id != NULL)
    {
      cJSON_AddStringToObject(root, "id", id);
    }

  cJSON_AddItemToObject(root, "data", data);
  if (cJSON_PrintPreallocated(root, s->output, sizeof(s->output), false))
    {
      ret = nyabula_eye_ws_send(fd, 1, s->output, strlen(s->output));
    }

  cJSON_Delete(root);
  return ret;
}

/****************************************************************************
 * Name: nyabula_eye_ws_error
 ****************************************************************************/

static int nyabula_eye_ws_error(int fd, struct nyabula_eye_ws_session_s *s,
                                const char *id, const char *topic,
                                const char *code)
{
  cJSON *data = cJSON_CreateObject();
  cJSON_AddStringToObject(data, "code", code);
  cJSON_AddStringToObject(data, "message", code);
  return nyabula_eye_ws_emit(fd, s, "err", id, topic, data);
}

/****************************************************************************
 * Name: nyabula_eye_ws_auth
 ****************************************************************************/

static bool nyabula_eye_ws_auth(const char *actual, const char *expected)
{
  unsigned int different = 0;
  if (actual == NULL || strlen(actual) != NYABULA_WS_TOKEN_SIZE)
    {
      return false;
    }

  for (size_t i = 0; i < NYABULA_WS_TOKEN_SIZE; i++)
    {
      different |= (unsigned char)actual[i] ^ (unsigned char)expected[i];
    }

  return different == 0;
}

/****************************************************************************
 * Name: nyabula_eye_ws_request
 ****************************************************************************/

static int nyabula_eye_ws_request(int fd, struct nyabula_eye_ws_session_s *s,
                                  const char *token)
{
  cJSON *root = cJSON_ParseWithOpts(s->message, NULL, true);
  cJSON *data;
  cJSON *result;
  const char *topic;
  const char *id;
  const char *type;
  struct nyabula_core_snapshot_s snapshot;
  int ret = -EINVAL;
  if (!cJSON_IsObject(root))
    {
      cJSON_Delete(root);
      return -EINVAL;
    }

  topic = nyabula_eye_ws_string(root, "topic");
  id = nyabula_eye_ws_string(root, "id");
  type = nyabula_eye_ws_string(root, "type");
  data = cJSON_GetObjectItemCaseSensitive(root, "data");
  cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
  if (topic == NULL || id == NULL || strlen(id) == 0 ||
      strlen(id) >= NYABULA_CORE_REQUEST_ID_MAX || strlen(topic) > 40 ||
      type == NULL || strcmp(type, "req") != 0 || !cJSON_IsObject(data) ||
      !cJSON_IsNumber(version) || version->valuedouble != 1)
    {
      goto out;
    }

  /* What a browser may ask before it has proved anything: whether there is
   * a password to enter, and to exchange one for a session token.  Neither
   * closes the connection on failure -- a mistyped password is not an
   * attack -- and guessing is slowed by the lock in ny_web_auth instead.
   */

  if (strcmp(topic, "sys.auth.state") == 0)
    {
      uint32_t retry = 0;
      bool locked = ny_web_auth_locked(&retry);
      result = cJSON_CreateObject();
      cJSON_AddBoolToObject(result, "passwordSet", ny_web_auth_has_password());
      cJSON_AddBoolToObject(result, "locked", locked);
      cJSON_AddNumberToObject(result, "retryAfterMs", retry);
      ret = nyabula_eye_ws_emit(fd, s, "res", id, topic, result);
      goto out;
    }

  if (strcmp(topic, "sys.login") == 0)
    {
      char session[NY_WEB_AUTH_TOKEN_SIZE + 1];
      uint32_t retry = 0;
      int status =
          ny_web_auth_check(nyabula_eye_ws_string(data, "password"), &retry);
      if (status == 0)
        {
          status = ny_web_auth_session_token(token, session);
        }

      if (status == 0)
        {
          result = cJSON_CreateObject();
          cJSON_AddStringToObject(result, "token", session);
          memset(session, 0, sizeof(session));
          ret = nyabula_eye_ws_emit(fd, s, "res", id, topic, result);
          memset(s->output, 0, sizeof(s->output));
        }
      else
        {
          cJSON *failure = cJSON_CreateObject();
          const char *code = status == -EAGAIN   ? "ELOCKED"
                             : status == -ENOENT ? "ENOPASSWORD"
                             : status == -EACCES ? "EAUTH"
                                                 : "EIO";
          cJSON_AddStringToObject(failure, "code", code);
          cJSON_AddStringToObject(failure, "message", code);
          if (retry > 0)
            {
              cJSON_AddNumberToObject(failure, "retryAfterMs", retry);
            }

          ret = nyabula_eye_ws_emit(fd, s, "err", id, topic, failure);
        }

      goto out;
    }

  if (strcmp(topic, "sys.hello") == 0)
    {
      /* Either credential opens the panel.  Which one it was is kept,
       * because it decides what changing the password requires.
       */

      const char *offered = nyabula_eye_ws_string(data, "token");
      char session[NY_WEB_AUTH_TOKEN_SIZE + 1];
      bool pair = nyabula_eye_ws_auth(offered, token);
      bool valid = pair;
      if (!valid && ny_web_auth_session_token(token, session) == 0)
        {
          valid = nyabula_eye_ws_auth(offered, session);
          memset(session, 0, sizeof(session));
        }

      if (!valid)
        {
          nyabula_eye_ws_error(fd, s, id, topic, "EACCES");
          ret = -EACCES;
          goto out;
        }

      s->pair_auth = pair;

      bool eye_ready = nyabula_eye_service_snapshot(&snapshot) == 0;
#ifndef CONFIG_NYABULA_CORE_PRODUCT
      if (!eye_ready)
        {
          nyabula_eye_ws_error(fd, s, id, topic, "ENODEV");
          ret = -ENODEV;
          goto out;
        }
#endif

      result = cJSON_CreateObject();
      cJSON *device = cJSON_AddObjectToObject(result, "device");
      cJSON_AddStringToObject(device, "id", "nyabula-native-eye");
#if defined(CONFIG_NYABULA_CORE_PRODUCT) && \
    defined(CONFIG_NYABULA_CORE_NETWORK)
      {
        /* The name the owner gave it, which is also what its access point
         * and the router call it.
         */

        char name[33] = "Nyabula";
        ny_product_network_name(name, sizeof(name));
        cJSON_AddStringToObject(device, "name", name);
      }
#else
      cJSON_AddStringToObject(device, "name", "Nyabula Core");
#endif
      cJSON_AddStringToObject(device, "coreVersion", "native-eye-v1");
#ifdef CONFIG_NYABULA_CORE_PRODUCT
      cJSON_AddStringToObject(result, "role", "owner");
#else
      cJSON_AddStringToObject(result, "role", "family");
#endif
      cJSON_AddBoolToObject(result, "eyeReady", eye_ready);
      cJSON_AddStringToObject(result, "auth", pair ? "pair" : "session");
      cJSON_AddBoolToObject(result, "passwordSet", ny_web_auth_has_password());
      cJSON *capabilities = cJSON_AddArrayToObject(result, "capabilities");
      cJSON_AddItemToArray(capabilities, cJSON_CreateString("eyes.native-v1"));
#ifdef CONFIG_NYABULA_CORE_PRODUCT
      cJSON_AddItemToArray(capabilities,
                           cJSON_CreateString("core.product-v1"));
      cJSON_AddItemToArray(capabilities, cJSON_CreateString("core.device-v1"));
      cJSON_AddItemToArray(capabilities, cJSON_CreateString("core.alarms-v1"));
      cJSON_AddItemToArray(capabilities,
                           cJSON_CreateString("core.notifications-v1"));
      cJSON_AddItemToArray(capabilities,
                           cJSON_CreateString("core.briefing-v1"));
#ifdef CONFIG_NYABULA_CORE_AGENT
      cJSON_AddItemToArray(capabilities,
                           cJSON_CreateString("core.companion-v1"));
#endif
#ifdef CONFIG_NYABULA_CORE_WEATHER
      cJSON_AddItemToArray(capabilities,
                           cJSON_CreateString("core.weather-v1"));
#endif
#ifdef CONFIG_NYABULA_CORE_MEDIA
      cJSON_AddItemToArray(capabilities, cJSON_CreateString("core.media-v1"));
#endif
#endif
      ret = nyabula_eye_ws_emit(fd, s, "res", id, topic, result);
      if (ret == 0 && !s->authenticated)
        {
          nxmutex_lock(&g_web_lock);
          g_web_panels++;
          nxmutex_unlock(&g_web_lock);
        }

      s->authenticated = ret == 0;
      s->published = false;
      goto out;
    }

  if (!s->authenticated)
    {
      nyabula_eye_ws_error(fd, s, id, topic, "EACCES");
      ret = -EACCES;
      goto out;
    }

  if (strcmp(topic, "sys.password.set") == 0)
    {
      /* Arriving with the pair token means having read it off the device,
       * and that is allowed to set a password outright -- it is how the
       * first one is chosen and how a forgotten one is replaced.  Arriving
       * with a session token proves only that this browser knew the
       * password once, so the current one is asked for again: a panel left
       * open on someone's desk must not be enough to lock its owner out.
       */

      char session[NY_WEB_AUTH_TOKEN_SIZE + 1];
      const char *code = NULL;
      int status = 0;
      if (!s->pair_auth && ny_web_auth_has_password())
        {
          uint32_t retry = 0;
          status = ny_web_auth_check(nyabula_eye_ws_string(data, "current"),
                                     &retry);
          code = status == -EAGAIN ? "ELOCKED" : "EAUTH";
        }

      if (status == 0)
        {
          status = ny_web_auth_set(nyabula_eye_ws_string(data, "password"));
          code = status == -EINVAL ? "EWEAK" : "EIO";
        }

      if (status == 0)
        {
          status = ny_web_auth_session_token(token, session);
          code = "EIO";
        }

      if (status == 0)
        {
          result = cJSON_CreateObject();
          cJSON_AddStringToObject(result, "token", session);
          memset(session, 0, sizeof(session));
          ret = nyabula_eye_ws_emit(fd, s, "res", id, topic, result);
          memset(s->output, 0, sizeof(s->output));
        }
      else
        {
          ret = nyabula_eye_ws_error(fd, s, id, topic, code);
        }

      goto out;
    }

  if (strcmp(topic, "sys.ping") == 0)
    {
      struct timespec now;
      clock_gettime(CLOCK_REALTIME, &now);
      result = cJSON_CreateObject();
      cJSON_AddNumberToObject(
          result, "t", (double)now.tv_sec * 1000 + now.tv_nsec / 1000000);
      ret = nyabula_eye_ws_emit(fd, s, "res", id, topic, result);
      goto out;
    }

  if (strcmp(topic, "eyes.state.get") == 0)
    {
      ret = nyabula_eye_service_snapshot(&snapshot);
      ret = ret < 0
                ? nyabula_eye_ws_error(fd, s, id, topic, "ENODEV")
                : nyabula_eye_ws_emit(fd, s, "res", id, topic,
                                      nyabula_eye_wire_snapshot(&snapshot));
      goto out;
    }

#ifdef CONFIG_NYABULA_CORE_PRODUCT
  /* The product layer also answers eyes.expression, for the agent: it shows
   * the feeling under the agent's name on a five second lease, which is
   * right for an emotion and wrong for a choice the owner made on the
   * panel.  That topic is this transport's own, so it is not offered to the
   * product layer at all.
   */

  const struct ny_product_caller_s caller = { "bootstrap", NY_PRODUCT_OWNER };
  result = NULL;
  ret = strcmp(topic, "eyes.expression") == 0
            ? -ENOSYS
            : ny_product_request(&caller, topic, data, &result);
  if (ret != -ENOSYS)
    {
      if (ret < 0)
        {
          cJSON_Delete(result);
          ret = nyabula_eye_ws_error(fd, s, id, topic, ny_web_error_name(ret));
        }
      else
        {
          ret = nyabula_eye_ws_emit(fd, s, "res", id, topic, result);
        }

      goto out;
    }
#endif

  bool allowed =
      strcmp(topic, "eyes.expression") == 0 ||
      strcmp(topic, "eyes.gaze") == 0 || strcmp(topic, "eyes.blink") == 0 ||
      strcmp(topic, "eyes.auto_blink") == 0 ||
      strcmp(topic, "eyes.ambient") == 0 || strcmp(topic, "eyes.iris") == 0 ||
      strcmp(topic, "eyes.scene.show") == 0 ||
      strcmp(topic, "eyes.scene.update") == 0 ||
      strcmp(topic, "eyes.scene.hide") == 0;
  if (!allowed)
    {
      ret = nyabula_eye_ws_error(fd, s, id, topic, "ENOTFOUND");
      goto out;
    }

  if (strcmp(topic, "eyes.gaze") == 0)
    {
      cJSON *x = cJSON_GetObjectItemCaseSensitive(data, "x");
      cJSON *y = cJSON_GetObjectItemCaseSensitive(data, "y");
      cJSON *hold = cJSON_GetObjectItemCaseSensitive(data, "hold_ms");
      if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) || !cJSON_IsNumber(hold) ||
          !isfinite(x->valuedouble) || !isfinite(y->valuedouble) ||
          !isfinite(hold->valuedouble) || fabs(x->valuedouble) > 1 ||
          fabs(y->valuedouble) > 1 || hold->valuedouble < 0 ||
          hold->valuedouble > NYABULA_WS_LEASE_MS ||
          floor(hold->valuedouble) != hold->valuedouble)
        {
          ret = nyabula_eye_ws_error(fd, s, id, topic, "EINVAL");
          goto out;
        }
    }

  /* Identity, priority and lease never come from the request. The transport
   * calls the same copied queue used by other Core workers, not the engine.
   *
   * What the owner picks stays picked: an expression, a scene, an iris or
   * ambient setting holds until it is changed, because a face that snaps
   * back to neutral five seconds after being told to look happy reads as
   * a fault.  A glance or a blink is a gesture and still expires.
   *
   * Holding for ever has a cost -- this source outranks the agent, so a
   * pinned expression would silence every emotion the agent shows from
   * then on.  Choosing the idle expression is therefore a release rather
   * than one more override: it gives the face back.
   */

  uint32_t lease_ms =
      strcmp(topic, "eyes.gaze") == 0 || strcmp(topic, "eyes.blink") == 0
          ? NYABULA_WS_LEASE_MS
          : 0;
  const char *action = topic;
  const char *chosen = nyabula_eye_ws_string(data, "expression");
  bool release = strcmp(topic, "eyes.expression") == 0 && chosen != NULL &&
                 strcmp(chosen, "idle") == 0;

  cJSON *command = cJSON_CreateObject();
  cJSON *params = release ? cJSON_CreateObject() : cJSON_Duplicate(data, true);
  if (release && params != NULL)
    {
      action = "core.release";
      cJSON_AddStringToObject(params, "domain", "expression");
    }

  if (command == NULL || params == NULL ||
      !cJSON_AddItemToObject(command, "params", params))
    {
      cJSON_Delete(command);
      cJSON_Delete(params);
      ret = -ENOMEM;
      goto out;
    }

  if (cJSON_AddStringToObject(command, "source", NYABULA_WS_SOURCE) == NULL ||
      cJSON_AddStringToObject(command, "id", id) == NULL ||
      cJSON_AddStringToObject(command, "action", action) == NULL ||
      cJSON_AddNumberToObject(command, "priority", NYABULA_WS_PRIORITY) ==
          NULL ||
      cJSON_AddNumberToObject(command, "lease_ms", lease_ms) == NULL ||
      !cJSON_PrintPreallocated(command, s->output, sizeof(s->output), false))
    {
      cJSON_Delete(command);
      ret = -ENOMEM;
      goto out;
    }

  ret = nyabula_eye_service_submit(NYABULA_WS_SOURCE, s->output,
                                   strlen(s->output));
  cJSON_Delete(command);
  if (ret < 0)
    {
      ret = nyabula_eye_ws_error(fd, s, id, topic,
                                 ret == -ENODEV   ? "ENODEV"
                                 : ret == -EAGAIN ? "EBUSY"
                                                  : "EINVAL");
      goto out;
    }

  result = cJSON_CreateObject();
  cJSON_AddBoolToObject(result, "queued", true);
  ret = nyabula_eye_ws_emit(fd, s, "res", id, topic, result);
out:
  cJSON_Delete(root);
  return ret;
}

/****************************************************************************
 * Name: nyabula_eye_ws_client
 ****************************************************************************/

static int nyabula_eye_ws_client(int fd, const char *token, const char *origin)
{
  struct nyabula_eye_ws_session_s *s = calloc(1, sizeof(*s));
  uint64_t started = nyabula_eye_ws_now();
  uint64_t last_input = started;
  uint64_t next_state = started;
  size_t body = 0;
  int ret;
  if (s == NULL)
    {
      return -ENOMEM;
    }

  ret = nyabula_eye_ws_upgrade(fd, origin, s->message, sizeof(s->message),
                               &body);
  if (ret == NYABULA_WS_NOT_UPGRADE)
    {
      /* An ordinary page request.  It is served and the connection ends; a
       * peer that went away half way through is not worth a log line.
       *
       * Whatever part of a request body arrived with the head is in the
       * same buffer, just past the head's terminator.
       */

      ny_web_http_serve(fd, s->message, s->message + strlen(s->message) + 1,
                        body, token);
      free(s);
      return -ECONNRESET;
    }

  while (ret == 0)
    {
      uint64_t now = nyabula_eye_ws_now();
      struct pollfd pfd = { fd, POLLIN, 0 };
      if ((!s->authenticated && now - started > NYABULA_WS_AUTH_MS) ||
          now - last_input > NYABULA_WS_IDLE_MS ||
          (s->fragmented && now > s->fragment_deadline))
        {
          break;
        }

      if (s->authenticated && now >= next_state)
        {
          struct nyabula_core_snapshot_s snapshot;
          next_state = now + NYABULA_WS_STATE_MS;
          if (nyabula_eye_service_snapshot(&snapshot) < 0)
            {
#ifdef CONFIG_NYABULA_CORE_PRODUCT
              s->published = false;
#else
              break;
#endif
            }
          else if (!s->published || s->revision != snapshot.revision)
            {
              ret = nyabula_eye_ws_emit(fd, s, "event", NULL, "eye.state",
                                        nyabula_eye_wire_snapshot(&snapshot));
              if (ret < 0)
                {
                  break;
                }

              s->revision = snapshot.revision;
              s->published = true;
            }
        }

      int ready = poll(&pfd, 1, NYABULA_WS_STATE_MS);
      if (ready < 0 && errno == EINTR)
        {
          continue;
        }

      if (ready < 0 || (ready > 0 && !(pfd.revents & POLLIN)))
        {
          break;
        }

      if (ready == 0)
        {
          continue;
        }

      uint8_t opcode;
      bool final;
      int count = nyabula_eye_ws_receive(fd, s->frame, sizeof(s->frame),
                                         &opcode, &final);
      if (count < 0)
        {
          ret = count;
          break;
        }

      last_input = now;
      if (opcode >= 8)
        {
          if (opcode == 8)
            {
              unsigned int code =
                  count >= 2 ? ((unsigned int)s->frame[0] << 8) | s->frame[1]
                             : 1000;
              bool valid_code =
                  (code >= 1000 && code <= 1014 && code != 1004 &&
                   code != 1005 && code != 1006) ||
                  (code >= 3000 && code <= 4999);
              if (count == 1 || !valid_code ||
                  (count > 2 && !nyabula_eye_ws_utf8(s->frame + 2, count - 2)))
                {
                  ret = -EPROTO;
                  break;
                }

              nyabula_eye_ws_send(fd, 8, s->frame, count);
              break;
            }

          if (opcode == 9)
            {
              ret = nyabula_eye_ws_send(fd, 10, s->frame, count);
            }

          continue;
        }

      if ((opcode == 0 && !s->fragmented) || (opcode == 1 && s->fragmented) ||
          (size_t)count > NYABULA_WS_MESSAGE_MAX - s->used)
        {
          ret = -EPROTO;
          break;
        }

      if (!s->fragmented)
        {
          s->fragment_deadline = now + NYABULA_WS_AUTH_MS;
        }

      memcpy(s->message + s->used, s->frame, count);
      s->used += count;
      s->fragmented = !final;
      if (!final)
        {
          continue;
        }

      if (memchr(s->message, '\0', s->used) != NULL ||
          !nyabula_eye_ws_utf8((unsigned char *)s->message, s->used) ||
          nyabula_eye_service_check_depth(s->message, s->used) < 0)
        {
          ret = -EINVAL;
          break;
        }

      s->message[s->used] = '\0';
      if (now - s->window >= 1000)
        {
          s->requests = 0;
          s->window = now;
        }

      if (++s->requests > NYABULA_WS_MAX_REQUESTS)
        {
          ret = -EBUSY;
          break;
        }

      ret = nyabula_eye_ws_request(fd, s, token);
      s->used = 0;
    }

  if (s->authenticated)
    {
      bool last;
      nxmutex_lock(&g_web_lock);
      last = --g_web_panels == 0;
      nxmutex_unlock(&g_web_lock);

      /* What the panel set holds for as long as a panel is open, and no
       * longer.  Without this a scene started from a page that was then
       * closed, or lost its connection, would sit on the eyes until the
       * next restart with nobody left to dismiss it.
       */

      if (last)
        {
          static const char release[] =
              "{\"action\":\"core.release\",\"id\":\"panel-closed\","
              "\"source\":\"" NYABULA_WS_SOURCE "\",\"priority\":40,"
              "\"lease_ms\":0,\"params\":{\"domain\":\"all\"}}";
          nyabula_eye_service_submit(NYABULA_WS_SOURCE, release,
                                     sizeof(release) - 1);
        }
    }

  free(s);
  return ret;
}

#ifdef CONFIG_NYABULA_CORE_PRODUCT
/****************************************************************************
 * Name: ny_web_error_name
 ****************************************************************************/

static const char *ny_web_error_name(int status)
{
  switch (-status)
    {
      case EACCES:
      case EPERM:
        return "EPERM";
      case ESTALE:
      case EEXIST:
        return "ECONFLICT";
      case EALREADY:
        return "EALREADY";
      case ECANCELED:
        return "ECANCELED";
      case ENOENT:
        return "ENOTFOUND";
      case ENODEV:
      case ENOTSUP:
        return "EUNAVAILABLE";
      case ENODATA:
        return "ENOTCONFIGURED";
      case E2BIG:
      case ENOSPC:
        return "EQUOTA";
      case ENOMEM:
        return "ENOMEM";
      case ETIMEDOUT:
        return "ETIMEDOUT";
      case EBUSY:
      case EAGAIN:
        return "EBUSY";
      case EIO:
      case EBADMSG:
        return "EIO";

        /* The refusals of update.apply.  Each asks something different of
         * the owner, so none of them may fold into EINVAL: acknowledge the
         * advanced targets, pick another slot, agree to write under a mounted
         * filesystem, or give up on a target that is never written.
         */

      case ENOKEY:
        return "EADVANCED";
      case ETXTBSY:
        return "ERUNNING";
      case ENOTEMPTY:
        return "EMOUNTED";
      case EXDEV:
        return "EBLOCKED";
      case EMEDIUMTYPE:
        return "ENOTIMAGE";
      case EFBIG:
        return "ETOOLARGE";
      default:
        return "EINVAL";
    }
}
#endif

/****************************************************************************
 * Name: ny_web_client_worker
 ****************************************************************************/

static void *ny_web_client_worker(void *argument)
{
  struct ny_web_client_args_s *args = argument;
  /* An empty origin is the product start's "same host" policy. */

  int status = nyabula_eye_ws_client(
      args->fd, args->token, args->origin[0] != '\0' ? args->origin : NULL);
  if (status < 0 && status != -ECONNRESET)
    {
      fprintf(stderr, "nyabula_web: client closed (%d)\n", status);
    }

  nxmutex_lock(&g_web_lock);
  close(args->fd);
  g_web_clients[args->slot] = -1;
  nxmutex_unlock(&g_web_lock);
  memset(args->token, 0, sizeof(args->token));
  free(args);
  return NULL;
}

/****************************************************************************
 * Name: ny_web_stop
 ****************************************************************************/

int ny_web_stop(void)
{
  int ret = nxmutex_lock(&g_web_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_web_running)
    {
      g_web_stopping = true;
    }

  nxmutex_unlock(&g_web_lock);
  return 0;
}

/****************************************************************************
 * Name: ny_web_product_token
 *
 * Description:
 *   Copy out the token of the running product service, or fail with
 *   -ENOENT if there is none.
 *
 *   It exists for the pairing QR and must not be passed on to anything a
 *   client can read.  Showing it on the eyes is sound because seeing the
 *   eyes means standing in front of the device, which is the whole of the
 *   ownership test a device without a keyboard can make; putting it in a
 *   status reply would hand it to whoever asks.
 *
 ****************************************************************************/

int ny_web_product_token(char *out, size_t size)
{
  int ret = nxmutex_lock(&g_web_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (size <= NYABULA_WS_TOKEN_SIZE || g_web_product_token[0] == '\0')
    {
      ret = size <= NYABULA_WS_TOKEN_SIZE ? -ENOBUFS : -ENOENT;
    }
  else
    {
      memcpy(out, g_web_product_token, NYABULA_WS_TOKEN_SIZE + 1);
    }

  nxmutex_unlock(&g_web_lock);
  return ret;
}

/****************************************************************************
 * Name: ny_web_panel_count
 *
 * Description:
 *   How many panels are open, counting only sockets that have
 *   authenticated.
 *
 ****************************************************************************/

int ny_web_panel_count(void)
{
  int count = 0;
  if (nxmutex_lock(&g_web_lock) == 0)
    {
      count = g_web_panels;
      nxmutex_unlock(&g_web_lock);
    }

  return count;
}

/****************************************************************************
 * Name: ny_web_client_limits
 *
 * Description:
 *   Bound how long a peer that went away can hold a client thread.  A phone
 *   that leaves the network mid reply never resets the connection, and a
 *   send with no timeout would keep its thread, and one of the few client
 *   slots, until the next restart.  Every option is best effort: a build
 *   without keepalive still gets the send timeout.
 *
 ****************************************************************************/

static void ny_web_client_limits(int fd)
{
  struct timeval timeout = { NY_WEB_SEND_TIMEOUT_S, 0 };
  int value = 1;

  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
#ifdef CONFIG_NET_TCP_KEEPALIVE
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value));
  value = NY_WEB_KEEPALIVE_IDLE_S;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &value, sizeof(value));
  value = NY_WEB_KEEPALIVE_INTERVAL_S;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &value, sizeof(value));
  value = NY_WEB_KEEPALIVE_PROBES;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &value, sizeof(value));
#endif
}

/****************************************************************************
 * Name: ny_web_store_wait
 *
 * Description:
 *   Wait, within reason, for the filesystem that holds the token.
 *
 *   The board mounts its stores from a work queue, and the init script that
 *   starts this service does not wait for it.  Looking for the token before
 *   the mount has happened finds nothing, and "nothing" is what a first
 *   boot looks like: the service would mint a token it cannot store, and
 *   every restart would sign the owner out.
 *
 *   The wait is bounded because a device whose store never appears still
 *   has to come up.  The first path component names the mount point.
 *
 ****************************************************************************/

static void ny_web_store_wait(const char *path)
{
  char root[32];
  struct statfs info;
  const char *slash = path[0] == '/' ? strchr(path + 1, '/') : NULL;
  if (slash == NULL || (size_t)(slash - path) >= sizeof(root))
    {
      return;
    }

  memcpy(root, path, slash - path);
  root[slash - path] = '\0';
  for (int i = 0; i < NY_WEB_STORE_WAIT_MS / 100; i++)
    {
      if (statfs(root, &info) == 0 && info.f_type == MSDOS_SUPER_MAGIC)
        {
          return;
        }

      usleep(100 * 1000);
    }

  fprintf(stderr, "nyabula_web: %s did not appear; continuing without it\n",
          root);
}

/****************************************************************************
 * Name: ny_web_token_load
 *
 * Description:
 *   Read the access token: exactly NYABULA_WS_TOKEN_SIZE hex digits, with
 *   an optional trailing newline.  token must hold NYABULA_WS_TOKEN_SIZE + 3
 *   bytes.
 *
 *   -ENOENT means only that the file is not there.  A file that exists and
 *   does not parse is -EINVAL, and the caller must not treat that as an
 *   invitation to mint a new one: replacing a token that someone wrote,
 *   however badly, locks out whoever is holding the original.
 *
 ****************************************************************************/

static int ny_web_token_load(const char *path, char *token)
{
  FILE *file = fopen(path, "r");
  if (file == NULL)
    {
      /* A missing directory on the way reports ENOTDIR, and means the same
       * thing as a missing file: nothing has been written here yet.
       */

      return errno == ENOENT || errno == ENOTDIR ? -ENOENT : -EIO;
    }

  size_t length = fread(token, 1, NYABULA_WS_TOKEN_SIZE + 3, file);
  bool read_failed = ferror(file) != 0;
  fclose(file);
  while (length > 0 &&
         (token[length - 1] == '\n' || token[length - 1] == '\r'))
    {
      length--;
    }

  if (read_failed || length != NYABULA_WS_TOKEN_SIZE)
    {
      return read_failed ? -EIO : -EINVAL;
    }

  for (size_t i = 0; i < length; i++)
    {
      if (!isxdigit((unsigned char)token[i]))
        {
          return -EINVAL;
        }
    }

  token[length] = '\0';
  return 0;
}

/****************************************************************************
 * Name: ny_web_token_create
 *
 * Description:
 *   Mint a token from the system entropy source and store it.
 *
 *   A token that could not be stored is still returned and the service
 *   still starts: it is good until the next boot, which keeps a device with
 *   an unwritable config store reachable instead of silent.  The cost is a
 *   new token after every restart, and the warning says so.
 *
 ****************************************************************************/

static int ny_web_token_create(const char *path, char *token)
{
  static const char digits[] = "0123456789abcdef";
  unsigned char raw[NYABULA_WS_TOKEN_SIZE / 2];
  char directory[128];
  const char *slash;
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  ssize_t count = read(fd, raw, sizeof(raw));
  close(fd);
  if (count != (ssize_t)sizeof(raw))
    {
      return -EIO;
    }

  for (size_t i = 0; i < sizeof(raw); i++)
    {
      token[i * 2] = digits[raw[i] >> 4];
      token[i * 2 + 1] = digits[raw[i] & 0x0f];
    }

  token[NYABULA_WS_TOKEN_SIZE] = '\0';

  /* One level is enough: the store partition is the parent and either
   * exists or is the reason this will fail.
   */

  slash = strrchr(path, '/');
  if (slash != NULL && slash != path &&
      (size_t)(slash - path) < sizeof(directory))
    {
      memcpy(directory, path, slash - path);
      directory[slash - path] = '\0';
      mkdir(directory, 0700);
    }

  bool stored = false;
  FILE *file = fopen(path, "w");
  if (file != NULL)
    {
      stored = fprintf(file, "%s\n", token) > 0;
      stored = fclose(file) == 0 && stored;
    }

  if (!stored)
    {
      fprintf(stderr,
              "nyabula_web: token not stored at %s (%d); it will change on "
              "the next start\n",
              path, errno);
      return 0;
    }

  return 0;
}

/****************************************************************************
 * Name: ny_web_run
 ****************************************************************************/

int ny_web_run(int argc, char **argv)
{
  char token[NYABULA_WS_TOKEN_SIZE + 3];
  const char *token_path;
  const char *origin;
  const char *bind_ip;
  char *end;
  struct sockaddr_in address;
  long port;
  int server;
  int one = 1;
  int ret;
  bool product = argc == 1;
  if (!product && argc != 4 && argc != 5)
    {
      fprintf(stderr,
              "Usage: nyabula_web\n"
              "       nyabula_web <port> <token-file> <origin> [bind-ip]\n");
      return EXIT_FAILURE;
    }

  if (product)
    {
      /* Started by the system with nothing to tell it.  Every address is
       * served because the device cannot know which one it will have --
       * the provisioning AP's today, a DHCP lease tomorrow -- and the
       * origin is left unset, which the handshake reads as "the page must
       * have come from the host it is now calling".
       */

      port = CONFIG_NYABULA_CORE_WEB_PORT;
      token_path = CONFIG_NYABULA_CORE_WEB_TOKEN_PATH;
      origin = NULL;
      bind_ip = "0.0.0.0";
      ny_web_store_wait(token_path);

#ifdef CONFIG_NYABULA_CORE_STATE_SQLITE
      /* The state database matters more than the token.  Opening it creates
       * its directory, and doing that before the partition is mounted plants
       * the directory in the pseudo filesystem instead -- right where the
       * mount point has to go.
       */

      ny_web_store_wait(CONFIG_NYABULA_CORE_STATE_DATABASE);
#endif
      ny_web_auth_load(CONFIG_NYABULA_CORE_WEB_AUTH_PATH);
    }
  else
    {
      port = strtol(argv[1], &end, 10);
      if (*end != '\0' || port < 1 || port > 65535 || strlen(argv[3]) > 255 ||
          (strncmp(argv[3], "http://", 7) != 0 &&
           strncmp(argv[3], "https://", 8) != 0))
        {
          return EXIT_FAILURE;
        }

      token_path = argv[2];
      origin = argv[3];
      bind_ip = argc == 5 ? argv[4] : NULL;
    }

  /* Only the product start mints a token.  An operator who names a file is
   * saying which secret to use, and quietly replacing a missing one would
   * leave them holding a token the service no longer accepts.
   */

  ret = ny_web_token_load(token_path, token);
  if (ret == -ENOENT && product)
    {
      ret = ny_web_token_create(token_path, token);
    }

  if (ret < 0)
    {
      fprintf(stderr, "nyabula_web: no usable token at %s: %d\n", token_path,
              ret);
      return EXIT_FAILURE;
    }

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind_ip != NULL && inet_pton(AF_INET, bind_ip, &address.sin_addr) != 1)
    {
      return EXIT_FAILURE;
    }
  server = socket(AF_INET, SOCK_STREAM, 0);
  if (server < 0)
    {
      return EXIT_FAILURE;
    }

  if (fcntl(server, F_SETFD, FD_CLOEXEC) < 0)
    {
      close(server);
      return EXIT_FAILURE;
    }

  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 ||
      listen(server, NY_WEB_MAX_CLIENTS) < 0)
    {
      close(server);
      return EXIT_FAILURE;
    }

  nxmutex_lock(&g_web_lock);
  if (g_web_running)
    {
      nxmutex_unlock(&g_web_lock);
      close(server);
      return EXIT_FAILURE;
    }

  g_web_running = true;
  g_web_stopping = false;
  if (product)
    {
      memcpy(g_web_product_token, token, sizeof(g_web_product_token));
    }

  for (int i = 0; i < NY_WEB_MAX_CLIENTS; i++)
    {
      g_web_clients[i] = -1;
    }

  nxmutex_unlock(&g_web_lock);
#ifdef CONFIG_NYABULA_CORE_PRODUCT
  if (ny_product_start() < 0)
    {
      nxmutex_lock(&g_web_lock);
      g_web_running = false;
      nxmutex_unlock(&g_web_lock);
      close(server);
      return EXIT_FAILURE;
    }
#endif

  printf("nyabula_web: %s:%ld, %s origin\n",
         bind_ip != NULL ? bind_ip : "127.0.0.1", port,
         origin != NULL ? origin : "same-host");
  for (;;)
    {
      nxmutex_lock(&g_web_lock);
      bool stop = g_web_stopping;
      nxmutex_unlock(&g_web_lock);
      if (stop)
        {
          break;
        }

      struct pollfd listener = { server, POLLIN, 0 };
      int ready = poll(&listener, 1, 100);
      if (ready < 0 && errno == EINTR)
        {
          continue;
        }

      if (ready < 0)
        {
          break;
        }

      if (ready == 0)
        {
          continue;
        }

      if ((listener.revents & POLLIN) == 0)
        {
          break;
        }

      int client = accept(server, NULL, NULL);
      if (client < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          break;
        }

      if (fcntl(client, F_SETFD, FD_CLOEXEC) < 0)
        {
          close(client);
          continue;
        }

      ny_web_client_limits(client);
      struct ny_web_client_args_s *args = calloc(1, sizeof(*args));
      if (args == NULL)
        {
          close(client);
          continue;
        }

      args->fd = client;
      strlcpy(args->token, token, sizeof(args->token));
      if (origin != NULL)
        {
          strlcpy(args->origin, origin, sizeof(args->origin));
        }
      nxmutex_lock(&g_web_lock);
      int slot;
      for (slot = 0; slot < NY_WEB_MAX_CLIENTS; slot++)
        {
          if (g_web_clients[slot] < 0)
            {
              break;
            }
        }

      int status = EBUSY;
      if (slot < NY_WEB_MAX_CLIENTS)
        {
          pthread_attr_t attributes;
          pthread_t thread;
          args->slot = slot;
          g_web_clients[slot] = client;
          pthread_attr_init(&attributes);
          pthread_attr_setstacksize(&attributes, 32768);
          pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
          status =
              pthread_create(&thread, &attributes, ny_web_client_worker, args);
          pthread_attr_destroy(&attributes);
          if (status != 0)
            {
              g_web_clients[slot] = -1;
            }
        }

      nxmutex_unlock(&g_web_lock);
      if (status != 0)
        {
          close(client);
          memset(args->token, 0, sizeof(args->token));
          free(args);
        }
    }

  memset(token, 0, sizeof(token));
  nxmutex_lock(&g_web_lock);
  memset(g_web_product_token, 0, sizeof(g_web_product_token));
  nxmutex_unlock(&g_web_lock);
  close(server);
  for (;;)
    {
      bool active = false;
      nxmutex_lock(&g_web_lock);
      for (int i = 0; i < NY_WEB_MAX_CLIENTS; i++)
        {
          if (g_web_clients[i] >= 0)
            {
              active = true;
              shutdown(g_web_clients[i], SHUT_RDWR);
            }
        }

      if (!active)
        {
          g_web_running = false;
          g_web_stopping = false;
        }

      nxmutex_unlock(&g_web_lock);
      if (!active)
        {
          break;
        }

      usleep(10000);
    }

  return EXIT_SUCCESS;
}
