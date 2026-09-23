/****************************************************************************
 * app/nyabula_core/ny_product_timesync.c
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

/* Network time for the product.
 *
 * The clock is asked for as soon as the station has an address, then every
 * six hours; while no server has answered the question is repeated after
 * 15 s, a minute, and then every five minutes.
 *
 * The product worker calls the tick ten times a second and must never wait
 * for the network, and resolving a name alone can take the resolver's full
 * timeout several times over.  So the tick only decides when, and the
 * asking is done by a thread that lives for one round of questions.
 *
 * This is its own small client rather than apps/netutils/ntpclient because
 * that one is a permanent daemon which sets the clock on its own: it cannot
 * say when it did so, accepts any time it is given, and leaves no place to
 * record where the time came from or to see that the RTC got it.
 */

#include "ny_product.h"
#include "ny_sntp.h"

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_NYABULA_CORE_TIMESYNC_SERVERS
#define CONFIG_NYABULA_CORE_TIMESYNC_SERVERS \
  "ntp.aliyun.com;ntp.tencent.com;cn.pool.ntp.org;pool.ntp.org"
#endif

#ifndef CONFIG_NYABULA_CORE_TIMESYNC_STACKSIZE
#define CONFIG_NYABULA_CORE_TIMESYNC_STACKSIZE 16384
#endif

#define NY_TIMESYNC_PERIOD_MS  21600000u /* Six hours between good answers */
#define NY_TIMESYNC_TIMEOUT_MS 3000u     /* How long one server is given */
#define NY_TIMESYNC_HOST_MAX   64

/* A time server has already set the clock in this boot and a later answer
 * is more than this away from it: one of the two is wrong, and one server's
 * word is not enough to say which.
 */

#define NY_TIMESYNC_SUSPECT_MS 3600000

/* Two servers agree when they are this close, which allows for the seconds
 * that pass between asking them.
 */

#define NY_TIMESYNC_AGREE_MS 5000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_timesync_s
{
  bool online;         /* What the last tick was told */
  bool busy;           /* A round of questions is in progress */
  bool stopping;       /* The product is shutting down */
  bool synced;         /* A server has answered in this boot */
  uint64_t next_at_ms; /* Monotonic time of the next round */
  unsigned int failures;
  int last_error;
  char server[NY_TIMESYNC_HOST_MAX];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static const char *ny_timesync_reason(int error);
static uint64_t ny_timesync_token(void);
static bool ny_timesync_stopping(void);
static int ny_timesync_query(const char *host, struct ny_sntp_reply_s *reply,
                             uint64_t *arrived_ms, uint64_t *roundtrip_ms);
static int ny_timesync_round(char *server, size_t size);
static void *ny_timesync_worker(void *argument);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_timesync_lock = NXMUTEX_INITIALIZER;
static struct ny_timesync_s g_timesync;

static const uint32_t g_timesync_backoff_ms[] = { 15000, 60000, 300000 };

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_timesync_reason
 ****************************************************************************/

static const char *ny_timesync_reason(int error)
{
  switch (error)
    {
      case -EADDRNOTAVAIL:
        return "name not resolved";
      case -ETIMEDOUT:
        return "no reply";
      case -ECONNREFUSED:
        return "refused, kiss-o'-death";
      case -EHOSTUNREACH:
        return "server clock not synchronised";
      case -ERANGE:
        return "time not believable";
      default:
        return "failed";
    }
}

/****************************************************************************
 * Name: ny_timesync_token
 *
 * Description:
 *   The value a reply has to echo.  Anyone can send a datagram that claims
 *   to come from a time server; only one that saw the request knows this.
 *
 ****************************************************************************/

static uint64_t ny_timesync_token(void)
{
  uint64_t token = 0;
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd >= 0)
    {
      if (read(fd, &token, sizeof(token)) != (ssize_t)sizeof(token))
        {
          token = 0;
        }

      close(fd);
    }

  /* Without a random source the request is still tied to its reply, only
   * less secretly.  Zero means "no token" to the parser.
   */

  token ^= ny_product_time_ms(true) * 0x9e3779b97f4a7c15ULL;
  return token != 0 ? token : 1;
}

/****************************************************************************
 * Name: ny_timesync_stopping
 ****************************************************************************/

static bool ny_timesync_stopping(void)
{
  bool stopping = true;
  if (nxmutex_lock(&g_timesync_lock) == 0)
    {
      stopping = g_timesync.stopping;
      nxmutex_unlock(&g_timesync_lock);
    }

  return stopping;
}

/****************************************************************************
 * Name: ny_timesync_query
 *
 * Description:
 *   Ask one server.  On success the reply is filled in, together with the
 *   monotonic time at which it arrived and how long the exchange took.
 *
 ****************************************************************************/

static int ny_timesync_query(const char *host, struct ny_sntp_reply_s *reply,
                             uint64_t *arrived_ms, uint64_t *roundtrip_ms)
{
  struct addrinfo hints;
  struct addrinfo *found = NULL;
  struct sockaddr_in server;
  uint8_t packet[NY_SNTP_PACKET_SIZE];
  uint8_t answer[128]; /* A reply may carry extension fields */
  uint64_t token = ny_timesync_token();
  uint64_t sent_ms;
  int ret;
  int fd;

  /* The interface gets an IPv4 lease and nothing else, so an IPv6 address
   * would be a server that cannot be reached.
   */

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(host, NULL, &hints, &found) != 0 || found == NULL)
    {
      return -EADDRNOTAVAIL;
    }

  if (found->ai_family != AF_INET || found->ai_addrlen < sizeof(server))
    {
      freeaddrinfo(found);
      return -EADDRNOTAVAIL;
    }

  memcpy(&server, found->ai_addr, sizeof(server));
  freeaddrinfo(found);
  server.sin_port = htons(NY_SNTP_PORT);

  fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    {
      return -errno;
    }

  /* Connected, so that only this server's datagrams are delivered. */

  if (connect(fd, (struct sockaddr *)&server, sizeof(server)) < 0)
    {
      ret = -errno;
      close(fd);
      return ret;
    }

  ny_sntp_build(packet, token);
  sent_ms = ny_product_time_ms(true);
  if (send(fd, packet, sizeof(packet), 0) != (ssize_t)sizeof(packet))
    {
      ret = -errno;
      close(fd);
      return ret;
    }

  for (;;)
    {
      struct pollfd wait;
      ssize_t length;
      uint64_t waited_ms = ny_product_time_ms(true) - sent_ms;
      if (waited_ms >= NY_TIMESYNC_TIMEOUT_MS)
        {
          ret = -ETIMEDOUT;
          break;
        }

      wait.fd = fd;
      wait.events = POLLIN;
      wait.revents = 0;
      ret = poll(&wait, 1, (int)(NY_TIMESYNC_TIMEOUT_MS - waited_ms));
      if (ret < 0 && errno != EINTR)
        {
          ret = -errno;
          break;
        }

      if (ret <= 0)
        {
          continue; /* The check at the top ends the wait */
        }

      length = recv(fd, answer, sizeof(answer), MSG_DONTWAIT);
      *arrived_ms = ny_product_time_ms(true);
      if (length < 0)
        {
          if (errno == EAGAIN || errno == EINTR)
            {
              continue;
            }

          ret = -errno;
          break;
        }

      ret = ny_sntp_parse(answer, (size_t)length, token,
                          (int64_t)ny_product_clock_floor_ms(),
                          (int64_t)NY_PRODUCT_CLOCK_MAX_MS, reply);

      /* Something that is not the answer to this request -- a late reply
       * to an earlier one, or not NTP at all -- is no reason to stop
       * listening for the one that is.
       */

      if (ret != -ESTALE && ret != -EMSGSIZE && ret != -EPROTO)
        {
          *roundtrip_ms = *arrived_ms - sent_ms;
          break;
        }
    }

  close(fd);
  return ret;
}

/****************************************************************************
 * Name: ny_timesync_round
 *
 * Description:
 *   Go through the servers until one gives a time that can be used, and
 *   set the clock from it.  Returns 0 and the server's name, or the last
 *   error.
 *
 ****************************************************************************/

static int ny_timesync_round(char *server, size_t size)
{
  char host[NY_TIMESYNC_HOST_MAX];
  char detail[NY_TIMESYNC_HOST_MAX + 48];
  bool doubted = false;
  int64_t doubted_now_ms = 0;
  uint64_t doubted_arrived_ms = 0;
  int result = -ENOENT;

  for (unsigned int index = 0;; index++)
    {
      struct ny_sntp_reply_s reply;
      uint64_t arrived_ms = 0;
      uint64_t roundtrip_ms = 0;
      int64_t server_now_ms;
      int64_t target_ms;
      int64_t off_ms;
      int ret = ny_sntp_server(CONFIG_NYABULA_CORE_TIMESYNC_SERVERS, index,
                               host, sizeof(host));
      if (ret == -ENOENT)
        {
          break;
        }

      if (ret < 0)
        {
          continue;
        }

      if (ny_timesync_stopping())
        {
          return -ECANCELED;
        }

      /* A query that fails before any reply arrives leaves this alone. */

      memset(&reply, 0, sizeof(reply));
      ret = ny_timesync_query(host, &reply, &arrived_ms, &roundtrip_ms);
      if (ret < 0)
        {
          syslog(LOG_WARNING, "nytime: %s: %s%s%s (%d)\n", host,
                 ny_timesync_reason(ret), reply.kiss[0] != '\0' ? " " : "",
                 reply.kiss, ret);
          result = ret;
          continue;
        }

      server_now_ms = ny_sntp_now_ms(&reply, roundtrip_ms);
      target_ms =
          server_now_ms + (int64_t)(ny_product_time_ms(true) - arrived_ms);
      off_ms = target_ms - (int64_t)ny_product_time_ms(false);
      if (ny_product_clock_source() == NY_PRODUCT_CLOCK_SNTP &&
          (off_ms > NY_TIMESYNC_SUSPECT_MS ||
           off_ms < -NY_TIMESYNC_SUSPECT_MS))
        {
          /* Two servers tell the same time when the difference between
           * their answers is the time that passed between them.
           */

          int64_t apart_ms = (server_now_ms - doubted_now_ms) -
                             (int64_t)(arrived_ms - doubted_arrived_ms);
          if (!doubted || apart_ms > NY_TIMESYNC_AGREE_MS ||
              apart_ms < -NY_TIMESYNC_AGREE_MS)
            {
              syslog(LOG_WARNING,
                     "nytime: %s is %lld s away from a clock "
                     "that a time server set; asking another\n",
                     host, (long long)(off_ms / 1000));
              doubted = true;
              doubted_now_ms = server_now_ms;
              doubted_arrived_ms = arrived_ms;
              result = -ERANGE;
              continue;
            }
        }

      snprintf(detail, sizeof(detail), "%s stratum %u rtt %llu ms", host,
               (unsigned int)reply.stratum, (unsigned long long)roundtrip_ms);
      ret = ny_product_clock_set((uint64_t)target_ms, NY_PRODUCT_CLOCK_SNTP,
                                 detail);
      if (ret < 0)
        {
          syslog(LOG_WARNING, "nytime: %s: clock not set (%d)\n", host, ret);
          result = ret;
          continue;
        }

      strlcpy(server, host, size);
      return 0;
    }

  return result;
}

/****************************************************************************
 * Name: ny_timesync_worker
 ****************************************************************************/

static void *ny_timesync_worker(void *argument)
{
  char server[NY_TIMESYNC_HOST_MAX] = "";
  int result = ny_timesync_round(server, sizeof(server));
  uint64_t now = ny_product_time_ms(true);
  (void)argument;

  nxmutex_lock(&g_timesync_lock);
  g_timesync.busy = false;
  g_timesync.last_error = result;
  if (result == 0)
    {
      g_timesync.synced = true;
      g_timesync.failures = 0;
      g_timesync.next_at_ms = now + NY_TIMESYNC_PERIOD_MS;
      strlcpy(g_timesync.server, server, sizeof(g_timesync.server));
    }
  else if (result != -ECANCELED)
    {
      unsigned int last =
          sizeof(g_timesync_backoff_ms) / sizeof(g_timesync_backoff_ms[0]) - 1;
      unsigned int step =
          g_timesync.failures < last ? g_timesync.failures : last;
      g_timesync.failures++;
      g_timesync.next_at_ms = now + g_timesync_backoff_ms[step];
      syslog(LOG_WARNING,
             "nytime: no usable answer (%d), attempt %u, next "
             "in %u s\n",
             result, g_timesync.failures,
             (unsigned int)(g_timesync_backoff_ms[step] / 1000));
    }

  nxmutex_unlock(&g_timesync_lock);
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_product_timesync_tick
 *
 * Description:
 *   Called from the network tick with whether the station has an address.
 *   Never waits for anything.
 *
 ****************************************************************************/

void ny_product_timesync_tick(bool online)
{
  pthread_attr_t attributes;
  pthread_t thread;
  uint64_t now = ny_product_time_ms(true);
  bool start;
  int ret;
  if (nxmutex_lock(&g_timesync_lock) < 0)
    {
      return;
    }

  if (g_timesync.stopping && !g_timesync.busy)
    {
      g_timesync.stopping = false; /* The product has been started again */
    }

  /* A link that has just come up is the moment to ask, whatever was
   * planned: it may be the first network since boot, or a different one.
   */

  if (online && !g_timesync.online)
    {
      g_timesync.next_at_ms = now;
      g_timesync.failures = 0;
    }

  g_timesync.online = online;
  start = online && !g_timesync.busy && !g_timesync.stopping &&
          now >= g_timesync.next_at_ms;
  if (start)
    {
      g_timesync.busy = true;
    }

  nxmutex_unlock(&g_timesync_lock);
  if (!start)
    {
      return;
    }

  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes,
                            CONFIG_NYABULA_CORE_TIMESYNC_STACKSIZE);
  pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  ret = pthread_create(&thread, &attributes, ny_timesync_worker, NULL);
  pthread_attr_destroy(&attributes);
  if (ret != 0)
    {
      syslog(LOG_ERR, "nytime: no worker thread (%d)\n", ret);
      nxmutex_lock(&g_timesync_lock);
      g_timesync.busy = false;
      g_timesync.last_error = -ret;
      g_timesync.next_at_ms = now + g_timesync_backoff_ms[0];
      nxmutex_unlock(&g_timesync_lock);
    }
}

/****************************************************************************
 * Name: ny_product_timesync_request
 *
 * Description:
 *   Ask now rather than when it is next due (system.time.sync).
 *
 ****************************************************************************/

void ny_product_timesync_request(void)
{
  if (nxmutex_lock(&g_timesync_lock) == 0)
    {
      g_timesync.next_at_ms = 0;
      g_timesync.failures = 0;
      nxmutex_unlock(&g_timesync_lock);
    }
}

/****************************************************************************
 * Name: ny_product_timesync_shutdown
 *
 * Description:
 *   A round in progress is not waited for -- it can sit in the resolver for
 *   a long time -- but it will not touch the clock once this has returned
 *   and it has noticed.
 *
 ****************************************************************************/

void ny_product_timesync_shutdown(void)
{
  if (nxmutex_lock(&g_timesync_lock) == 0)
    {
      g_timesync.stopping = true;
      g_timesync.online = false;
      nxmutex_unlock(&g_timesync_lock);
    }
}

/****************************************************************************
 * Name: ny_product_timesync_describe
 *
 * Description:
 *   Add a "sync" object to a result.  False means out of memory.
 *
 ****************************************************************************/

bool ny_product_timesync_describe(cJSON *root)
{
  struct ny_timesync_s state;
  uint64_t now = ny_product_time_ms(true);
  const char *text;
  cJSON *sync;
  if (nxmutex_lock(&g_timesync_lock) < 0)
    {
      return false;
    }

  state = g_timesync;
  nxmutex_unlock(&g_timesync_lock);
  text = state.busy      ? "querying"
         : !state.online ? "offline"
         : state.synced  ? "synced"
                         : "waiting";
  sync = cJSON_AddObjectToObject(root, "sync");
  return sync != NULL &&
         cJSON_AddStringToObject(sync, "state", text) != NULL &&
         cJSON_AddStringToObject(sync, "server", state.server) != NULL &&
         cJSON_AddNumberToObject(sync, "error", state.last_error) != NULL &&
         cJSON_AddNumberToObject(sync, "failures", state.failures) != NULL &&
         cJSON_AddNumberToObject(sync, "next_in_ms",
                                 state.next_at_ms > now
                                     ? (double)(state.next_at_ms - now)
                                     : 0) != NULL;
}
