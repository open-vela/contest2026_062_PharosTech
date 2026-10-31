/****************************************************************************
 * app/nyabula_core/ny_cloud.c
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

#include "ny_cloud.h"
#include "ny_product_store.h"
#include "ny_web.h"
#include "ny_websocket.h"
#include <errno.h>
#include <fcntl.h>
#include <libwebsockets.h>
#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <nyabula_eye_service.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NY_CLOUD_DOMAIN      "cloud"
#define NY_CLOUD_IDENTITY    "/config/nyabula/cloud.identity"
#define NY_CLOUD_URL_MAX     512
#define NY_CLOUD_MESSAGE_MAX 65536
#define NY_CLOUD_CHANNELS    8
#define NY_CLOUD_QUEUE       64
#define NY_CLOUD_TICK_US     50000

struct ny_cloud_worker_s;
struct ny_cloud_channel_s
{
  struct ny_cloud_worker_s *worker;
  char sid[32];
  void *session;
};

struct ny_cloud_worker_s
{
  struct lws_context *context;
  struct lws *socket;
  lws_sorted_usec_list_t timer;
  struct ny_cloud_channel_s channels[NY_CLOUD_CHANNELS];
  unsigned char *queue[NY_CLOUD_QUEUE];
  size_t lengths[NY_CLOUD_QUEUE];
  unsigned int head;
  unsigned int count;
  char receive[NY_CLOUD_MESSAGE_MAX + 1];
  size_t used;
  uint64_t last_receive;
  char id[21];
  char secret[65];
  char claim[7];
  char error[96];
  bool finish;
  bool failed;
  bool registered;
  unsigned int generation;
};

static mutex_t g_cloud_lock = NXMUTEX_INITIALIZER;
static pthread_t g_cloud_thread;
static bool g_cloud_started;
static bool g_cloud_stop;
static unsigned int g_cloud_generation;
static char g_cloud_state[24] = "disabled";
static char g_cloud_detail[96];
static char g_cloud_id[21];
static char g_cloud_claim[7];

static const char *ny_cloud_string(const cJSON *root, const char *name);
static void ny_cloud_state(const char *state, const char *detail);
static int ny_cloud_identity(struct ny_cloud_worker_s *worker);
static int ny_cloud_url(char *url, const char **host, const char **path,
                        int *port, int *tls);
static void ny_cloud_channels_close(struct ny_cloud_worker_s *worker);
static int ny_cloud_enqueue(struct ny_cloud_worker_s *worker, cJSON *message);
static int ny_cloud_send(void *context, const char *data, size_t length);
static int ny_cloud_close_channel(struct ny_cloud_worker_s *worker,
                                  const char *sid);
static int ny_cloud_receive(struct ny_cloud_worker_s *worker);
static int ny_cloud_callback(struct lws *socket,
                             enum lws_callback_reasons reason, void *user,
                             void *input, size_t length);
static void ny_cloud_tick(lws_sorted_usec_list_t *timer);
static void *ny_cloud_worker(void *argument);

static const struct lws_protocols g_cloud_protocols[] = {
  { "nyabula-cloud", ny_cloud_callback, 0, NY_CLOUD_MESSAGE_MAX, 0, NULL, 0 },
  LWS_PROTOCOL_LIST_TERM
};

/****************************************************************************
 * Name: ny_cloud_string
 ****************************************************************************/

static const char *ny_cloud_string(const cJSON *root, const char *name)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsString(value) ? value->valuestring : NULL;
}

/****************************************************************************
 * Name: ny_cloud_state
 ****************************************************************************/

static void ny_cloud_state(const char *state, const char *detail)
{
  nxmutex_lock(&g_cloud_lock);
  strlcpy(g_cloud_state, state, sizeof(g_cloud_state));
  strlcpy(g_cloud_detail, detail != NULL ? detail : "",
          sizeof(g_cloud_detail));
  nxmutex_unlock(&g_cloud_lock);
}

/****************************************************************************
 * Name: ny_cloud_identity
 ****************************************************************************/

static int ny_cloud_identity(struct ny_cloud_worker_s *worker)
{
  unsigned char entropy[44];
  char record[128];
  char *end;
  int fd = open(NY_CLOUD_IDENTITY, O_RDONLY);
  int ret = 0;
  if (fd < 0 && errno == ENOENT)
    {
      if (rename(NY_CLOUD_IDENTITY ".new", NY_CLOUD_IDENTITY) == 0)
        {
          fd = open(NY_CLOUD_IDENTITY, O_RDONLY);
        }
    }

  if (fd >= 0)
    {
      ssize_t size = read(fd, record, sizeof(record) - 1);
      close(fd);
      if (size <= 0)
        {
          return -EIO;
        }

      record[size] = 0;
      if (sscanf(record, "%20s %64s %6s", worker->id, worker->secret,
                 worker->claim) != 3 ||
          strlen(worker->id) != 20 || strlen(worker->secret) != 64 ||
          strlen(worker->claim) != 6)
        {
          return -EINVAL;
        }

      strtoul(worker->claim, &end, 10);
      return *end == 0 ? 0 : -EINVAL;
    }

  if (errno != ENOENT)
    {
      return -errno;
    }

  fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  ssize_t size = read(fd, entropy, sizeof(entropy));
  close(fd);
  if (size != sizeof(entropy))
    {
      return -EIO;
    }

  memcpy(worker->id, "nya-", 4);
  for (unsigned int i = 0; i < 8; i++)
    {
      snprintf(worker->id + 4 + i * 2, 3, "%02x", entropy[i]);
    }

  for (unsigned int i = 0; i < 32; i++)
    {
      snprintf(worker->secret + i * 2, 3, "%02x", entropy[i + 8]);
    }

  uint32_t number;
  memcpy(&number, entropy + 40, sizeof(number));
  snprintf(worker->claim, sizeof(worker->claim), "%06lu",
           (unsigned long)(number % 1000000));
  memset(entropy, 0, sizeof(entropy));
  int length = snprintf(record, sizeof(record), "%s\n%s\n%s\n", worker->id,
                        worker->secret, worker->claim);
  fd = open(NY_CLOUD_IDENTITY ".new", O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    {
      ret = -errno;
    }
  else
    {
      if (write(fd, record, length) != length || fsync(fd) < 0)
        {
          ret = -EIO;
        }

      close(fd);
      if (ret == 0 && rename(NY_CLOUD_IDENTITY ".new", NY_CLOUD_IDENTITY) < 0)
        {
          ret = -errno;
        }
    }

  memset(record, 0, sizeof(record));
  return ret;
}

/****************************************************************************
 * Name: ny_cloud_url
 ****************************************************************************/

static int ny_cloud_url(char *url, const char **host, const char **path,
                        int *port, int *tls)
{
  const char *protocol;
  if (strpbrk(url, "\r\n\t @?#") != NULL ||
      (strncmp(url, "ws://", 5) != 0 && strncmp(url, "wss://", 6) != 0))
    {
      return -EINVAL;
    }

  if (lws_parse_uri(url, &protocol, host, port, path) != 0 || (*host)[0] == 0)
    {
      return -EINVAL;
    }

  *tls = strcmp(protocol, "wss") == 0 ? LCCSCF_USE_SSL : 0;
  return *port > 0 && *port <= 65535 ? 0 : -EINVAL;
}

/****************************************************************************
 * Name: ny_cloud_channels_close
 ****************************************************************************/

static void ny_cloud_channels_close(struct ny_cloud_worker_s *worker)
{
  for (unsigned int i = 0; i < NY_CLOUD_CHANNELS; i++)
    {
      ny_web_relay_close(worker->channels[i].session);
      memset(&worker->channels[i], 0, sizeof(worker->channels[i]));
    }
}

/****************************************************************************
 * Name: ny_cloud_enqueue
 ****************************************************************************/

static int ny_cloud_enqueue(struct ny_cloud_worker_s *worker, cJSON *message)
{
  char *text = message == NULL ? NULL : cJSON_PrintUnformatted(message);
  cJSON_Delete(message);
  if (text == NULL)
    {
      return -ENOMEM;
    }

  size_t size = strlen(text);
  if (size > NY_CLOUD_MESSAGE_MAX || worker->count == NY_CLOUD_QUEUE)
    {
      free(text);
      return -ENOBUFS;
    }

  unsigned char *buffer = malloc(LWS_PRE + size);
  if (buffer == NULL)
    {
      free(text);
      return -ENOMEM;
    }

  memcpy(buffer + LWS_PRE, text, size);
  memset(text, 0, size);
  free(text);
  unsigned int slot = (worker->head + worker->count) % NY_CLOUD_QUEUE;
  worker->queue[slot] = buffer;
  worker->lengths[slot] = size;
  worker->count++;
  lws_callback_on_writable(worker->socket);
  return 0;
}

/****************************************************************************
 * Name: ny_cloud_send
 ****************************************************************************/

static int ny_cloud_send(void *context, const char *data, size_t length)
{
  struct ny_cloud_channel_s *channel = context;
  cJSON *message = cJSON_CreateObject();
  if (message == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddStringToObject(message, "op", "frame");
  cJSON_AddStringToObject(message, "sid", channel->sid);
  cJSON_AddRawToObject(message, "frame", data);
  return ny_cloud_enqueue(channel->worker, message);
}

/****************************************************************************
 * Name: ny_cloud_close_channel
 ****************************************************************************/

static int ny_cloud_close_channel(struct ny_cloud_worker_s *worker,
                                  const char *sid)
{
  cJSON *message = cJSON_CreateObject();
  if (message == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddStringToObject(message, "op", "close");
  cJSON_AddStringToObject(message, "sid", sid);
  return ny_cloud_enqueue(worker, message);
}

/****************************************************************************
 * Name: ny_cloud_receive
 ****************************************************************************/

static int ny_cloud_receive(struct ny_cloud_worker_s *worker)
{
  if (nyabula_eye_service_check_depth(worker->receive, worker->used) < 0)
    {
      return -EINVAL;
    }

  cJSON *root = cJSON_ParseWithOpts(worker->receive, NULL, true);
  const char *op = ny_cloud_string(root, "op");
  const char *sid = ny_cloud_string(root, "sid");
  int ret = 0;
  if (op == NULL)
    {
      cJSON_Delete(root);
      return -EINVAL;
    }

  if (strcmp(op, "registered") == 0)
    {
      worker->registered = true;
      ny_cloud_state("connected", "");
    }
  else if (strcmp(op, "ping") == 0)
    {
      cJSON *reply = cJSON_CreateObject();
      cJSON_AddStringToObject(reply, "op", "pong");
      ret = ny_cloud_enqueue(worker, reply);
    }
  else if (strcmp(op, "pong") == 0)
    {
      /* The transport also verifies its WebSocket ping responses. */
    }
  else if (strcmp(op, "err") == 0)
    {
      ny_cloud_state("error", "cloud rejected device registration");
      ret = -EACCES;
    }
  else if (!worker->registered || sid == NULL || strlen(sid) >= 32)
    {
      ret = -EPROTO;
    }
  else
    {
      struct ny_cloud_channel_s *channel = NULL;
      struct ny_cloud_channel_s *empty = NULL;
      for (unsigned int i = 0; i < NY_CLOUD_CHANNELS; i++)
        {
          if (worker->channels[i].session == NULL)
            {
              empty = &worker->channels[i];
            }
          else if (strcmp(worker->channels[i].sid, sid) == 0)
            {
              channel = &worker->channels[i];
            }
        }

      if (strcmp(op, "open") == 0)
        {
          if (channel != NULL || empty == NULL)
            {
              ret = ny_cloud_close_channel(worker, sid);
            }
          else
            {
              empty->worker = worker;
              strlcpy(empty->sid, sid, sizeof(empty->sid));
              empty->session = ny_web_relay_open(ny_cloud_send, empty);
              ret = empty->session != NULL ? 0 : -ENOMEM;
            }
        }
      else if (strcmp(op, "close") == 0 && channel != NULL)
        {
          ny_web_relay_close(channel->session);
          memset(channel, 0, sizeof(*channel));
        }
      else if (strcmp(op, "frame") == 0 && channel != NULL)
        {
          cJSON *frame = cJSON_GetObjectItemCaseSensitive(root, "frame");
          char *text =
              cJSON_IsObject(frame) ? cJSON_PrintUnformatted(frame) : NULL;
          if (text == NULL)
            {
              ret = -EINVAL;
            }
          else
            {
              int status =
                  ny_web_relay_receive(channel->session, text, strlen(text));
              memset(text, 0, strlen(text));
              free(text);
              if (status < 0)
                {
                  ret = ny_cloud_close_channel(worker, channel->sid);
                  ny_web_relay_close(channel->session);
                  memset(channel, 0, sizeof(*channel));
                }
            }
        }
    }

  cJSON_Delete(root);
  return ret;
}

/****************************************************************************
 * Name: ny_cloud_callback
 ****************************************************************************/

static int ny_cloud_callback(struct lws *socket,
                             enum lws_callback_reasons reason, void *user,
                             void *input, size_t length)
{
  struct ny_cloud_worker_s *worker = user;
  switch (reason)
    {
      case LWS_CALLBACK_CLIENT_ESTABLISHED:
        {
          worker->socket = socket;
          cJSON *message = cJSON_CreateObject();
          cJSON_AddStringToObject(message, "op", "register");
          cJSON_AddStringToObject(message, "deviceId", worker->id);
          cJSON_AddStringToObject(message, "secret", worker->secret);
          cJSON_AddStringToObject(message, "claimCode", worker->claim);
          cJSON_AddStringToObject(message, "name", "Nyabula");
          cJSON_AddStringToObject(message, "coreVersion", "native-eye-v1");
          return ny_cloud_enqueue(worker, message) < 0 ? -1 : 0;
        }

      case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (worker->count > 0)
          {
            unsigned int slot = worker->head;
            unsigned char *buffer = worker->queue[slot];
            size_t size = worker->lengths[slot];
            int written =
                lws_write(socket, buffer + LWS_PRE, size, LWS_WRITE_TEXT);
            memset(buffer, 0, LWS_PRE + size);
            free(buffer);
            worker->queue[slot] = NULL;
            worker->head = (slot + 1) % NY_CLOUD_QUEUE;
            worker->count--;
            if (written != size)
              {
                return -1;
              }

            if (worker->count > 0)
              {
                lws_callback_on_writable(socket);
              }
          }

        break;

      case LWS_CALLBACK_CLIENT_RECEIVE:
        worker->last_receive = nyabula_eye_ws_now();
        if (lws_frame_is_binary(socket) ||
            length > NY_CLOUD_MESSAGE_MAX - worker->used ||
            memchr(input, 0, length) != NULL)
          {
            return -1;
          }

        memcpy(worker->receive + worker->used, input, length);
        worker->used += length;
        if (lws_is_final_fragment(socket) &&
            lws_remaining_packet_payload(socket) == 0)
          {
            worker->receive[worker->used] = 0;
            int ret = ny_cloud_receive(worker);
            memset(worker->receive, 0, worker->used);
            worker->used = 0;
            return ret < 0 ? -1 : 0;
          }

        break;

      case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        if (input != NULL)
          {
            snprintf(worker->error, sizeof(worker->error), "%.*s",
                     (int)(length < sizeof(worker->error) - 1
                               ? length
                               : sizeof(worker->error) - 1),
                     (const char *)input);
          }

        /* Fall through: connection errors release every virtual session. */

      case LWS_CALLBACK_CLIENT_CLOSED:
        worker->socket = NULL;
        worker->failed = true;
        worker->finish = true;
        ny_cloud_channels_close(worker);
        break;

      default:
        break;
    }

  return 0;
}

/****************************************************************************
 * Name: ny_cloud_tick
 ****************************************************************************/

static void ny_cloud_tick(lws_sorted_usec_list_t *timer)
{
  struct ny_cloud_worker_s *worker =
      lws_container_of(timer, struct ny_cloud_worker_s, timer);
  nxmutex_lock(&g_cloud_lock);
  worker->finish = worker->finish || g_cloud_stop ||
                   worker->generation != g_cloud_generation;
  nxmutex_unlock(&g_cloud_lock);
  if (nyabula_eye_ws_now() - worker->last_receive >
      (worker->registered ? 75000 : 15000))
    {
      worker->failed = true;
      worker->finish = true;
    }

  if (worker->finish)
    {
      return;
    }

  for (unsigned int i = 0; i < NY_CLOUD_CHANNELS; i++)
    {
      struct ny_cloud_channel_s *channel = &worker->channels[i];
      if (channel->session != NULL && ny_web_relay_tick(channel->session) < 0)
        {
          if (ny_cloud_close_channel(worker, channel->sid) < 0)
            {
              worker->finish = worker->failed = true;
            }
          ny_web_relay_close(channel->session);
          memset(channel, 0, sizeof(*channel));
        }
    }

  lws_sul_schedule(worker->context, 0, timer, ny_cloud_tick, NY_CLOUD_TICK_US);
}

/****************************************************************************
 * Name: ny_cloud_worker
 ****************************************************************************/

static void *ny_cloud_worker(void *argument)
{
  struct ny_cloud_worker_s *worker = calloc(1, sizeof(*worker));
  unsigned int retry = 0;
  if (worker == NULL || ny_cloud_identity(worker) < 0)
    {
      ny_cloud_state("error", "cannot load persistent cloud identity");
      free(worker);
      return NULL;
    }

  nxmutex_lock(&g_cloud_lock);
  strlcpy(g_cloud_id, worker->id, sizeof(g_cloud_id));
  strlcpy(g_cloud_claim, worker->claim, sizeof(g_cloud_claim));
  nxmutex_unlock(&g_cloud_lock);
  for (;;)
    {
      nxmutex_lock(&g_cloud_lock);
      bool stop = g_cloud_stop;
      worker->generation = g_cloud_generation;
      nxmutex_unlock(&g_cloud_lock);
      if (stop)
        {
          break;
        }

      cJSON *settings = NULL;
      uint64_t revision;
      int status =
          ny_product_store_read(NY_CLOUD_DOMAIN, &settings, &revision);
      const cJSON *enabled =
          cJSON_GetObjectItemCaseSensitive(settings, "enabled");
      const char *setting = ny_cloud_string(settings, "url");
      char url[NY_CLOUD_URL_MAX + 1];
      strlcpy(url, setting != NULL ? setting : "", sizeof(url));
      bool active = status == 0 && cJSON_IsTrue(enabled);
      cJSON_Delete(settings);
      if (!active)
        {
          ny_cloud_state(status < 0 ? "error" : "disabled",
                         status < 0 ? "cannot read cloud configuration" : "");
          usleep(250000);
          continue;
        }

      const char *host;
      const char *path;
      int port;
      int tls;
      if (ny_cloud_url(url, &host, &path, &port, &tls) < 0)
        {
          ny_cloud_state("error", "invalid cloud WebSocket URL");
          usleep(250000);
          continue;
        }

      char request_path[NY_CLOUD_URL_MAX + 2];
      snprintf(request_path, sizeof(request_path), "/%s",
               path[0] != 0 && strcmp(path, "/") != 0
                   ? path
                   : "api/v1/device/connect");
      struct lws_context_creation_info info;
      memset(&info, 0, sizeof(info));
      info.port = CONTEXT_PORT_NO_LISTEN;
      info.protocols = g_cloud_protocols;
      info.options = tls != 0 ? LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT : 0;
      info.fd_limit_per_thread = 4;
      if (tls != 0)
        {
          info.client_ssl_ca_filepath = CONFIG_NYABULA_CORE_CLOUD_CA;
        }
      worker->last_receive = nyabula_eye_ws_now();
      worker->finish = false;
      worker->failed = false;
      worker->registered = false;
      worker->error[0] = 0;
      worker->context = lws_create_context(&info);
      if (worker->context == NULL)
        {
          ny_cloud_state("error", "cannot initialize cloud TLS transport");
          usleep(1000000);
          continue;
        }

      struct lws_client_connect_info connect;
      memset(&connect, 0, sizeof(connect));
      connect.context = worker->context;
      connect.address = host;
      connect.host = host;
      connect.port = port;
      connect.path = request_path;
      connect.ssl_connection = tls;
      connect.local_protocol_name = "nyabula-cloud";
      connect.userdata = worker;
      connect.pwsi = &worker->socket;
      ny_cloud_state("connecting", "");
      if (lws_client_connect_via_info(&connect) == NULL)
        {
          worker->finish = true;
          worker->failed = true;
        }

      lws_sul_schedule(worker->context, 0, &worker->timer, ny_cloud_tick,
                       NY_CLOUD_TICK_US);
      while (!worker->finish && lws_service(worker->context, 0) >= 0)
        {
        }

      lws_sul_cancel(&worker->timer);
      lws_context_destroy(worker->context);
      worker->context = NULL;
      worker->socket = NULL;
      ny_cloud_channels_close(worker);
      for (unsigned int i = 0; i < NY_CLOUD_QUEUE; i++)
        {
          if (worker->queue[i] != NULL)
            {
              memset(worker->queue[i], 0, LWS_PRE + worker->lengths[i]);
              free(worker->queue[i]);
              worker->queue[i] = NULL;
            }
        }

      worker->head = worker->count = worker->used = 0;
      if (worker->registered)
        {
          retry = 0;
        }

      if (worker->failed)
        {
          ny_cloud_state("reconnecting",
                         worker->error[0] != 0
                             ? worker->error
                             : "cloud connection lost; retrying");
          unsigned int wait = 1u << (retry < 5 ? retry++ : 5);
          for (unsigned int i = 0; i < wait * 4; i++)
            {
              nxmutex_lock(&g_cloud_lock);
              bool changed =
                  g_cloud_stop || worker->generation != g_cloud_generation;
              nxmutex_unlock(&g_cloud_lock);
              if (changed)
                {
                  break;
                }

              usleep(250000);
            }
        }
    }

  memset(worker, 0, sizeof(*worker));
  free(worker);
  ny_cloud_state("disabled", "");
  return NULL;
}

/****************************************************************************
 * Name: ny_cloud_start
 ****************************************************************************/

int ny_cloud_start(void)
{
  pthread_attr_t attr;
  nxmutex_lock(&g_cloud_lock);
  if (g_cloud_started)
    {
      nxmutex_unlock(&g_cloud_lock);
      return 0;
    }

  g_cloud_stop = false;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 32768);
  int ret = pthread_create(&g_cloud_thread, &attr, ny_cloud_worker, NULL);
  pthread_attr_destroy(&attr);
  g_cloud_started = ret == 0;
  nxmutex_unlock(&g_cloud_lock);
  return -ret;
}

/****************************************************************************
 * Name: ny_cloud_stop
 ****************************************************************************/

void ny_cloud_stop(void)
{
  nxmutex_lock(&g_cloud_lock);
  bool started = g_cloud_started;
  g_cloud_stop = true;
  nxmutex_unlock(&g_cloud_lock);
  if (started)
    {
      pthread_join(g_cloud_thread, NULL);
      nxmutex_lock(&g_cloud_lock);
      g_cloud_started = false;
      nxmutex_unlock(&g_cloud_lock);
    }
}

/****************************************************************************
 * Name: ny_cloud_config
 ****************************************************************************/

int ny_cloud_config(const cJSON *update, cJSON **result, bool owner)
{
  cJSON *settings = NULL;
  uint64_t revision;
  int ret = ny_product_store_read(NY_CLOUD_DOMAIN, &settings, &revision);
  if (ret < 0)
    {
      return ret;
    }

  bool enabled =
      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings, "enabled"));
  const char *old_url = ny_cloud_string(settings, "url");
  char url[NY_CLOUD_URL_MAX + 1];
  strlcpy(url, old_url != NULL ? old_url : "", sizeof(url));
  cJSON_Delete(settings);
  if (update != NULL)
    {
      if (!owner)
        {
          return -EACCES;
        }

      const cJSON *want = cJSON_GetObjectItemCaseSensitive(update, "enabled");
      const cJSON *where = cJSON_GetObjectItemCaseSensitive(update, "url");
      if ((want != NULL && !cJSON_IsBool(want)) ||
          (where != NULL && !cJSON_IsString(where)))
        {
          return -EINVAL;
        }

      if (want != NULL)
        {
          enabled = cJSON_IsTrue(want);
        }

      if (where != NULL)
        {
          if (strlen(where->valuestring) > NY_CLOUD_URL_MAX)
            {
              return -EINVAL;
            }

          strlcpy(url, where->valuestring, sizeof(url));
        }

      if (url[0] != 0 || enabled)
        {
          char check[sizeof(url)];
          const char *host;
          const char *path;
          int port;
          int tls;
          strlcpy(check, url, sizeof(check));
          if (ny_cloud_url(check, &host, &path, &port, &tls) < 0)
            {
              return -EINVAL;
            }
        }

      settings = cJSON_CreateObject();
      cJSON_AddBoolToObject(settings, "enabled", enabled);
      cJSON_AddStringToObject(settings, "url", url);
      ret = ny_product_store_write(NY_CLOUD_DOMAIN, settings, revision,
                                   &revision);
      cJSON_Delete(settings);
      if (ret < 0)
        {
          return ret;
        }

      nxmutex_lock(&g_cloud_lock);
      g_cloud_generation++;
      nxmutex_unlock(&g_cloud_lock);
    }

  cJSON *root = cJSON_CreateObject();
  if (root == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddBoolToObject(root, "enabled", enabled);
  cJSON_AddStringToObject(root, "url", url);
  nxmutex_lock(&g_cloud_lock);
  cJSON_AddBoolToObject(root, "connected",
                        strcmp(g_cloud_state, "connected") == 0);
  cJSON_AddStringToObject(root, "state", g_cloud_state);
  cJSON_AddStringToObject(root, "detail", g_cloud_detail);
  cJSON_AddStringToObject(root, "deviceId", g_cloud_id);
  if (owner)
    {
      cJSON_AddStringToObject(root, "claimCode", g_cloud_claim);
    }

  nxmutex_unlock(&g_cloud_lock);
  *result = root;
  return 0;
}
