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
#include "ny_websocket.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
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
#include <time.h>
#include <unistd.h>

#define NYABULA_WS_TOKEN_SIZE   64
#define NYABULA_WS_SOURCE       "webui"
#define NYABULA_WS_PRIORITY     40
#define NYABULA_WS_LEASE_MS     5000
#define NYABULA_WS_STATE_MS     50
#define NYABULA_WS_IDLE_MS      20000
#define NYABULA_WS_AUTH_MS      5000
#define NYABULA_WS_MAX_REQUESTS 40
#define NY_WEB_MAX_CLIENTS      4

static mutex_t g_web_lock = NXMUTEX_INITIALIZER;
static bool g_web_running;
static bool g_web_stopping;
static int g_web_clients[NY_WEB_MAX_CLIENTS];

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

  if (strcmp(topic, "sys.hello") == 0)
    {
      if (!nyabula_eye_ws_auth(nyabula_eye_ws_string(data, "token"), token))
        {
          nyabula_eye_ws_error(fd, s, id, topic, "EACCES");
          ret = -EACCES;
          goto out;
        }

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
      cJSON_AddStringToObject(device, "name", "Nyabula Core");
      cJSON_AddStringToObject(device, "coreVersion", "native-eye-v1");
#ifdef CONFIG_NYABULA_CORE_PRODUCT
      cJSON_AddStringToObject(result, "role", "owner");
#else
      cJSON_AddStringToObject(result, "role", "family");
#endif
      cJSON_AddBoolToObject(result, "eyeReady", eye_ready);
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
  const struct ny_product_caller_s caller = { "bootstrap", NY_PRODUCT_OWNER };
  result = NULL;
  ret = ny_product_request(&caller, topic, data, &result);
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
   */

  cJSON *command = cJSON_CreateObject();
  cJSON *params = cJSON_Duplicate(data, true);
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
      cJSON_AddStringToObject(command, "action", topic) == NULL ||
      cJSON_AddNumberToObject(command, "priority", NYABULA_WS_PRIORITY) ==
          NULL ||
      cJSON_AddNumberToObject(command, "lease_ms", NYABULA_WS_LEASE_MS) ==
          NULL ||
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
  int ret;
  if (s == NULL)
    {
      return -ENOMEM;
    }

  ret = nyabula_eye_ws_upgrade(fd, origin, s->message, sizeof(s->message));
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
  int status = nyabula_eye_ws_client(args->fd, args->token, args->origin);
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
 * Name: ny_web_run
 ****************************************************************************/

int ny_web_run(int argc, char **argv)
{
  char token[NYABULA_WS_TOKEN_SIZE + 3];
  char *end;
  struct sockaddr_in address;
  long port;
  FILE *file;
  int server;
  int one = 1;
  if (argc != 4 && argc != 5)
    {
      fprintf(
          stderr,
          "Usage: nyabula_eye_ws <port> <token-file> <origin> [bind-ip]\n");
      return EXIT_FAILURE;
    }

  port = strtol(argv[1], &end, 10);
  if (*end != '\0' || port < 1 || port > 65535 || strlen(argv[3]) > 255 ||
      (strncmp(argv[3], "http://", 7) != 0 &&
       strncmp(argv[3], "https://", 8) != 0))
    {
      return EXIT_FAILURE;
    }

  file = fopen(argv[2], "r");
  if (file == NULL)
    {
      return EXIT_FAILURE;
    }

  size_t length = fread(token, 1, sizeof(token), file);
  bool read_failed = ferror(file) != 0;
  fclose(file);
  while (length > 0 &&
         (token[length - 1] == '\n' || token[length - 1] == '\r'))
    {
      length--;
    }

  if (read_failed || length != NYABULA_WS_TOKEN_SIZE)
    {
      return EXIT_FAILURE;
    }

  for (size_t i = 0; i < length; i++)
    {
      if (!isxdigit((unsigned char)token[i]))
        {
          return EXIT_FAILURE;
        }
    }

  token[length] = '\0';
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (argc == 5 && inet_pton(AF_INET, argv[4], &address.sin_addr) != 1)
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

  printf("Nyabula Eye WS: %s:%ld, explicit token and Origin\n",
         argc == 5 ? argv[4] : "127.0.0.1", port);
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

      struct ny_web_client_args_s *args = calloc(1, sizeof(*args));
      if (args == NULL)
        {
          close(client);
          continue;
        }

      args->fd = client;
      strlcpy(args->token, token, sizeof(args->token));
      strlcpy(args->origin, argv[3], sizeof(args->origin));
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
