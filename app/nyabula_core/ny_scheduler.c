/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_scheduler.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ny_health.h"
#include "ny_permission.h"
#include "ny_runtime.h"
#include "ny_scheduler.h"
#include "ny_wasm.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_PLUGIN_ID_SIZE 64
#define NY_MINUTE_NS      60000000000ull
#define NY_HTTP_POLL_NS   10000000ull

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_scheduler_event_s
{
  char payload[CONFIG_NYABULA_CORE_EVENT_SIZE];
};

struct ny_scheduler_completion_s
{
  uint64_t token;
  int status;
  size_t length;
  char payload[CONFIG_NYABULA_CORE_EVENT_SIZE];
};

struct ny_scheduler_slot_s
{
  bool occupied;
  bool stopping;
  bool task_stopped;
  bool accept_completions;
  uint32_t generation;
  size_t completion_head;
  size_t completion_count;
  struct ny_scheduler_completion_s completions[NY_PLUGIN_MAX_PENDING];
  pid_t pid;
#ifdef CONFIG_BUILD_KERNEL
  pthread_t thread;
#endif
  int start_result;
  int exit_result;
  enum ny_plugin_state_e state;
  struct ny_plugin_config_s config;
  char id[NY_PLUGIN_ID_SIZE];
  char path[PATH_MAX];
  char index_arg[12];
  sem_t ready;
  sem_t start_consumed;
  sem_t pending;
  sem_t stopped;
  size_t head;
  size_t tail;
  size_t count;
  uint64_t event_window_ns;
  uint32_t event_count;
  struct ny_scheduler_event_s events[CONFIG_NYABULA_CORE_EVENT_DEPTH];
  struct ny_plugin_s plugin;
  struct ny_wasm_plugin_s wasm;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_scheduler_lock = NXMUTEX_INITIALIZER;
static struct ny_scheduler_slot_s
    g_scheduler_slots[CONFIG_NYABULA_CORE_MAX_PLUGINS];

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int ny_scheduler_wait(sem_t *sem);
static uint64_t ny_scheduler_now_ns(void);
static const char *ny_scheduler_state_name(enum ny_plugin_state_e state);
static struct ny_scheduler_slot_s *ny_scheduler_find_locked(const char *id);
static struct ny_scheduler_slot_s *ny_scheduler_empty_locked(void);
static int ny_scheduler_background_count_locked(void);
static int ny_scheduler_enqueue_locked(struct ny_scheduler_slot_s *slot,
                                       const char *payload);
static int ny_scheduler_worker(int argc, char *argv[]);
static int ny_scheduler_pump(void *opaque, uint64_t deadline_ns);
static int ny_scheduler_wasm_pump(void *opaque, uint64_t deadline_ns);
#ifdef CONFIG_BUILD_KERNEL
static void *ny_scheduler_pthread(void *argument);
#endif
static void ny_scheduler_clear_locked(struct ny_scheduler_slot_s *slot);
static int ny_scheduler_start_config(const struct ny_plugin_config_s *config);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int ny_scheduler_wait(sem_t *sem)
{
  int ret;

  do
    {
      ret = sem_wait(sem);
    }
  while (ret < 0 && errno == EINTR);

  return ret < 0 ? -errno : 0;
}

static uint64_t ny_scheduler_now_ns(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static const char *ny_scheduler_state_name(enum ny_plugin_state_e state)
{
  switch (state)
    {
      case NY_PLUGIN_EMPTY:
        return "empty";
      case NY_PLUGIN_LOADED:
        return "loaded";
      case NY_PLUGIN_RUNNING:
        return "running";
      case NY_PLUGIN_STOPPED:
        return "stopped";
      case NY_PLUGIN_FAILED:
        return "failed";
      default:
        return "unknown";
    }
}

static struct ny_scheduler_slot_s *ny_scheduler_find_locked(const char *id)
{
  int index;

  for (index = 0; index < CONFIG_NYABULA_CORE_MAX_PLUGINS; index++)
    {
      if (g_scheduler_slots[index].occupied &&
          strcmp(g_scheduler_slots[index].id, id) == 0)
        {
          return &g_scheduler_slots[index];
        }
    }

  return NULL;
}

static struct ny_scheduler_slot_s *ny_scheduler_empty_locked(void)
{
  int index;

  for (index = 0; index < CONFIG_NYABULA_CORE_MAX_PLUGINS; index++)
    {
      if (!g_scheduler_slots[index].occupied)
        {
          return &g_scheduler_slots[index];
        }
    }

  return NULL;
}

static int ny_scheduler_background_count_locked(void)
{
  int count = 0;
  int index;

  for (index = 0; index < CONFIG_NYABULA_CORE_MAX_PLUGINS; index++)
    {
      if (g_scheduler_slots[index].occupied &&
          g_scheduler_slots[index].config.background)
        {
          count++;
        }
    }

  return count;
}

static int ny_scheduler_enqueue_locked(struct ny_scheduler_slot_s *slot,
                                       const char *payload)
{
  struct ny_scheduler_event_s *event;

  if (slot->count >= CONFIG_NYABULA_CORE_EVENT_DEPTH)
    {
      return -EAGAIN;
    }

  event = &slot->events[slot->tail];
  memset(event, 0, sizeof(*event));
  if (payload != NULL)
    {
      strlcpy(event->payload, payload, sizeof(event->payload));
    }

  slot->tail = (slot->tail + 1) % CONFIG_NYABULA_CORE_EVENT_DEPTH;
  slot->count++;
  sem_post(&slot->pending);
  return 0;
}

static int ny_scheduler_pump(void *opaque, uint64_t deadline_ns)
{
  struct ny_scheduler_slot_s *slot = opaque;
  struct ny_scheduler_completion_s completion;
  struct timespec deadline;
  uint64_t wake_ns;
  int ret;

  for (;;)
    {
      nxmutex_lock(&g_scheduler_lock);
      if (slot->stopping)
        {
          nxmutex_unlock(&g_scheduler_lock);
          return -ECANCELED;
        }

      if (deadline_ns != 0 && ny_scheduler_now_ns() >= deadline_ns)
        {
          nxmutex_unlock(&g_scheduler_lock);
          return -ETIMEDOUT;
        }

      if (slot->completion_count != 0)
        {
          completion = slot->completions[slot->completion_head];
          slot->completion_head =
              (slot->completion_head + 1) % NY_PLUGIN_MAX_PENDING;
          slot->completion_count--;
          nxmutex_unlock(&g_scheduler_lock);
          ret = ny_plugin_async_deliver(&slot->plugin, completion.token,
                                        completion.status, completion.payload,
                                        completion.length);
          if (ret == -ENOENT || ret == -ESTALE)
            {
              continue;
            }

          return ret;
        }

      nxmutex_unlock(&g_scheduler_lock);
#ifdef CONFIG_NYABULA_CORE_HTTP
      ret = ny_plugin_http_poll(&slot->plugin);
      if (ret != -EAGAIN)
        {
          return ret;
        }
#endif
      if (deadline_ns == 0)
        {
          return -EAGAIN;
        }

      wake_ns = deadline_ns;
#ifdef CONFIG_NYABULA_CORE_HTTP
      if (ny_plugin_http_pending(&slot->plugin) &&
          wake_ns > ny_scheduler_now_ns() + NY_HTTP_POLL_NS)
        {
          wake_ns = ny_scheduler_now_ns() + NY_HTTP_POLL_NS;
        }
#endif
      deadline.tv_sec = wake_ns / 1000000000ull;
      deadline.tv_nsec = wake_ns % 1000000000ull;
      ret = sem_clockwait(&slot->pending, CLOCK_MONOTONIC, &deadline);
      if (ret < 0 && errno == ETIMEDOUT && wake_ns < deadline_ns)
        {
          continue;
        }
      if (ret < 0 && errno != EINTR)
        {
          return -errno;
        }
    }
}

static int ny_scheduler_wasm_pump(void *opaque, uint64_t deadline_ns)
{
  struct ny_scheduler_slot_s *slot = opaque;
  struct timespec wake;
  uint64_t wake_ns;
  bool stopping;
  int ret;
  for (;;)
    {
      nxmutex_lock(&g_scheduler_lock);
      stopping = slot->stopping;
      nxmutex_unlock(&g_scheduler_lock);
      if (stopping)
        {
          return -ECANCELED;
        }
      if (deadline_ns != 0 && ny_scheduler_now_ns() >= deadline_ns)
        {
          return -ETIMEDOUT;
        }
      ret = ny_wasm_poll(&slot->wasm);
      if (ret != -EAGAIN || deadline_ns == 0)
        {
          return ret;
        }
      wake_ns = deadline_ns;
      if (ny_wasm_pending(&slot->wasm) &&
          wake_ns > ny_scheduler_now_ns() + NY_HTTP_POLL_NS)
        {
          wake_ns = ny_scheduler_now_ns() + NY_HTTP_POLL_NS;
        }
      wake.tv_sec = wake_ns / 1000000000ull;
      wake.tv_nsec = wake_ns % 1000000000ull;
      ret = sem_clockwait(&slot->pending, CLOCK_MONOTONIC, &wake);
      if (ret < 0 && errno != ETIMEDOUT && errno != EINTR)
        {
          return -errno;
        }
    }
}

static int ny_scheduler_worker(int argc, char *argv[])
{
  struct ny_scheduler_event_s event;
  struct ny_scheduler_slot_s *slot;
  int index;
  int ret;

  if (argc < 2)
    {
      return EXIT_FAILURE;
    }

  index = atoi(argv[1]);
  if (index < 0 || index >= CONFIG_NYABULA_CORE_MAX_PLUGINS)
    {
      return EXIT_FAILURE;
    }

  slot = &g_scheduler_slots[index];
  if (slot->config.runtime == NY_PLUGIN_RUNTIME_WAMR)
    {
      ret = ny_wasm_load(&slot->wasm, &slot->config);
      if (ret >= 0)
        {
          slot->wasm.pump = ny_scheduler_wasm_pump;
          slot->wasm.pump_opaque = slot;
          ret = ny_wasm_start(&slot->wasm);
        }
    }
  else
    {
      ret = ny_plugin_load_config(&slot->plugin, &slot->config);
      if (ret >= 0)
        {
          slot->plugin.pump = ny_scheduler_pump;
          slot->plugin.pump_opaque = slot;
          nxmutex_lock(&g_scheduler_lock);
          slot->generation = slot->plugin.generation;
          slot->accept_completions = !slot->stopping;
          nxmutex_unlock(&g_scheduler_lock);
          ret = ny_plugin_start(&slot->plugin);
        }
    }

  nxmutex_lock(&g_scheduler_lock);
  slot->start_result = ret;
  slot->state = slot->config.runtime == NY_PLUGIN_RUNTIME_WAMR
                    ? ret < 0 ? NY_PLUGIN_FAILED : NY_PLUGIN_RUNNING
                    : slot->plugin.state;
  nxmutex_unlock(&g_scheduler_lock);
  sem_post(&slot->ready);

  while (ret >= 0)
    {
      ret = slot->config.runtime == NY_PLUGIN_RUNTIME_WAMR
                ? ny_scheduler_wasm_pump(slot, 0)
                : ny_scheduler_pump(slot, 0);
      if (ret < 0 && ret != -EAGAIN && ret != -ECANCELED)
        {
          break;
        }

      nxmutex_lock(&g_scheduler_lock);
      if (slot->stopping)
        {
          nxmutex_unlock(&g_scheduler_lock);
          ret = slot->config.runtime == NY_PLUGIN_RUNTIME_WAMR
                    ? ny_wasm_stop(&slot->wasm)
                    : ny_plugin_stop(&slot->plugin);
          break;
        }

      if (slot->count == 0)
        {
          nxmutex_unlock(&g_scheduler_lock);
#ifdef CONFIG_NYABULA_CORE_HTTP
          if (slot->config.runtime == NY_PLUGIN_RUNTIME_WAMR
                  ? ny_wasm_pending(&slot->wasm)
                  : ny_plugin_http_pending(&slot->plugin))
            {
              struct timespec wake;
              uint64_t wake_ns = ny_scheduler_now_ns() + NY_HTTP_POLL_NS;
              wake.tv_sec = wake_ns / 1000000000ull;
              wake.tv_nsec = wake_ns % 1000000000ull;
              ret = sem_clockwait(&slot->pending, CLOCK_MONOTONIC, &wake);
              if (ret < 0 && errno != ETIMEDOUT && errno != EINTR)
                {
                  ret = -errno;
                  break;
                }
              ret = 0;
              continue;
            }
#endif
          ret = ny_scheduler_wait(&slot->pending);
          continue;
        }

      event = slot->events[slot->head];
      slot->head = (slot->head + 1) % CONFIG_NYABULA_CORE_EVENT_DEPTH;
      slot->count--;
      nxmutex_unlock(&g_scheduler_lock);

      ret = slot->config.runtime == NY_PLUGIN_RUNTIME_WAMR
                ? ny_wasm_dispatch(&slot->wasm, event.payload)
                : ny_plugin_dispatch(&slot->plugin, event.payload);
      nxmutex_lock(&g_scheduler_lock);
      slot->state = slot->config.runtime == NY_PLUGIN_RUNTIME_WAMR
                        ? ret < 0 ? NY_PLUGIN_FAILED : NY_PLUGIN_RUNNING
                        : slot->plugin.state;
      nxmutex_unlock(&g_scheduler_lock);
    }

  nxmutex_lock(&g_scheduler_lock);
  slot->accept_completions = false;
  if (ret == -ECANCELED && slot->stopping)
    {
      ret = 0;
    }

  slot->exit_result = ret;
  slot->state = ret < 0 ? NY_PLUGIN_FAILED : NY_PLUGIN_STOPPED;
  nxmutex_unlock(&g_scheduler_lock);

  if (ret < 0)
    {
      ny_health_record_failure(&slot->config);
    }
  else
    {
      ny_health_record_success(&slot->config);
    }

  if (slot->config.runtime == NY_PLUGIN_RUNTIME_WAMR)
    {
      ny_wasm_destroy(&slot->wasm);
    }
  else
    {
      ny_plugin_destroy(&slot->plugin);
    }

  nxmutex_lock(&g_scheduler_lock);
  slot->task_stopped = true;
  nxmutex_unlock(&g_scheduler_lock);
  sem_post(&slot->stopped);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

#ifdef CONFIG_BUILD_KERNEL
static void *ny_scheduler_pthread(void *argument)
{
  char *argv[3];
  int ret;

  argv[0] = "nycore-worker";
  argv[1] = argument;
  argv[2] = NULL;
  ret = ny_scheduler_worker(2, argv);
  return (void *)(intptr_t)ret;
}
#endif

static void ny_scheduler_clear_locked(struct ny_scheduler_slot_s *slot)
{
  sem_destroy(&slot->ready);
  sem_destroy(&slot->start_consumed);
  sem_destroy(&slot->pending);
  sem_destroy(&slot->stopped);
  memset(slot, 0, sizeof(*slot));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_scheduler_start(const char *id, const char *path)
{
  struct ny_plugin_config_s config;

  if (id == NULL || path == NULL || strlen(id) >= sizeof(config.id))
    {
      return -EINVAL;
    }

  ny_manifest_default_config(&config, path);
  strlcpy(config.id, id, sizeof(config.id));
  return ny_scheduler_start_config(&config);
}

int ny_scheduler_start_package(const char *package_path)
{
  struct ny_plugin_config_s config;
  int ret;

  ret = ny_manifest_load(package_path, &config);
  return ret < 0 ? ret : ny_scheduler_start_config(&config);
}

int ny_scheduler_start_installed(const char *package_path,
                                 const char *storage_root)
{
  struct ny_plugin_config_s config;
  int ret;

  if (storage_root == NULL || storage_root[0] == '\0' ||
      strlen(storage_root) >= sizeof(config.storage_root))
    {
      return -EINVAL;
    }

  ret = ny_manifest_load(package_path, &config);
  if (ret >= 0)
    {
      strlcpy(config.storage_root, storage_root, sizeof(config.storage_root));
      ret = ny_scheduler_start_config(&config);
    }

  return ret;
}

int ny_scheduler_refresh_permissions(const char *id)
{
  struct ny_scheduler_slot_s *slot;
  struct ny_plugin_config_s config;
  pid_t pid;
  int ret;

  if (id == NULL)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_scheduler_lock);
  slot = ny_scheduler_find_locked(id);
  if (slot == NULL)
    {
      nxmutex_unlock(&g_scheduler_lock);
      return 0;
    }

  config = slot->config;
  pid = slot->pid;
  nxmutex_unlock(&g_scheduler_lock);

  ret = ny_permission_apply(&config);
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_scheduler_lock);
  slot = ny_scheduler_find_locked(id);
  if (slot != NULL && slot->pid == pid)
    {
      slot->config.permissions = config.permissions;
      if (slot->config.runtime == NY_PLUGIN_RUNTIME_WAMR)
        {
          ny_wasm_set_permissions(&slot->wasm, config.permissions);
          sem_post(&slot->pending);
        }
      else if (slot->accept_completions)
        {
          atomic_fetch_add(&slot->plugin.permission_generation, 1);
          atomic_store(&slot->plugin.permissions, config.permissions);
          sem_post(&slot->pending);
        }
    }

  nxmutex_unlock(&g_scheduler_lock);
  return 0;
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int ny_scheduler_start_config(const struct ny_plugin_config_s *config)
{
  struct ny_scheduler_slot_s *slot;
#ifdef CONFIG_BUILD_KERNEL
  struct sched_param parameters;
  pthread_attr_t attributes;
#else
  char *worker_argv[2];
#endif
  int index;
  int ret;

  if (config == NULL || config->id[0] == '\0' || config->entry[0] == '\0' ||
      strlen(config->id) >= NY_PLUGIN_ID_SIZE ||
      strlen(config->entry) >= PATH_MAX || config->events_per_minute == 0)
    {
      return -EINVAL;
    }

  ret = ny_health_check(config);
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_scheduler_lock);
  if (ny_scheduler_find_locked(config->id) != NULL)
    {
      nxmutex_unlock(&g_scheduler_lock);
      return -EEXIST;
    }

  if (config->background && ny_scheduler_background_count_locked() >=
                                CONFIG_NYABULA_CORE_MAX_BACKGROUND_PLUGINS)
    {
      nxmutex_unlock(&g_scheduler_lock);
      return -EDQUOT;
    }

  slot = ny_scheduler_empty_locked();
  if (slot == NULL)
    {
      nxmutex_unlock(&g_scheduler_lock);
      return -ENOSPC;
    }

  index = slot - g_scheduler_slots;
  memset(slot, 0, sizeof(*slot));
  slot->occupied = true;
  slot->state = NY_PLUGIN_EMPTY;
  slot->config = *config;
  strlcpy(slot->id, config->id, sizeof(slot->id));
  strlcpy(slot->path, config->entry, sizeof(slot->path));
  snprintf(slot->index_arg, sizeof(slot->index_arg), "%d", index);

  if (sem_init(&slot->ready, 0, 0) < 0)
    {
      ret = -errno;
      memset(slot, 0, sizeof(*slot));
      nxmutex_unlock(&g_scheduler_lock);
      return ret;
    }

  if (sem_init(&slot->pending, 0, 0) < 0)
    {
      ret = -errno;
      sem_destroy(&slot->ready);
      memset(slot, 0, sizeof(*slot));
      nxmutex_unlock(&g_scheduler_lock);
      return ret;
    }

  if (sem_init(&slot->stopped, 0, 0) < 0)
    {
      ret = -errno;
      sem_destroy(&slot->pending);
      sem_destroy(&slot->ready);
      memset(slot, 0, sizeof(*slot));
      nxmutex_unlock(&g_scheduler_lock);
      return ret;
    }

  if (sem_init(&slot->start_consumed, 0, 0) < 0)
    {
      ret = -errno;
      sem_destroy(&slot->stopped);
      sem_destroy(&slot->pending);
      sem_destroy(&slot->ready);
      memset(slot, 0, sizeof(*slot));
      nxmutex_unlock(&g_scheduler_lock);
      return ret;
    }

#ifdef CONFIG_BUILD_KERNEL
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, CONFIG_NYABULA_CORE_STACKSIZE);
  pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attributes, SCHED_FIFO);
  memset(&parameters, 0, sizeof(parameters));
  parameters.sched_priority = config->background
                                  ? CONFIG_NYABULA_CORE_BACKGROUND_PRIORITY
                                  : CONFIG_NYABULA_CORE_PRIORITY;
  pthread_attr_setschedparam(&attributes, &parameters);
  ret = pthread_create(&slot->thread, &attributes, ny_scheduler_pthread,
                       slot->index_arg);
  pthread_attr_destroy(&attributes);
  if (ret != 0)
    {
      ret = -ret;
      ny_scheduler_clear_locked(slot);
      nxmutex_unlock(&g_scheduler_lock);
      return ret;
    }

  slot->pid = (pid_t)slot->thread;
#else
  worker_argv[0] = slot->index_arg;
  worker_argv[1] = NULL;
  slot->pid = task_create(
      config->id,
      config->background ? CONFIG_NYABULA_CORE_BACKGROUND_PRIORITY
                         : CONFIG_NYABULA_CORE_PRIORITY,
      CONFIG_NYABULA_CORE_STACKSIZE, ny_scheduler_worker, worker_argv);
  if (slot->pid < 0)
    {
      ret = -errno;
      ny_scheduler_clear_locked(slot);
      nxmutex_unlock(&g_scheduler_lock);
      return ret;
    }
#endif

  nxmutex_unlock(&g_scheduler_lock);
  ret = ny_scheduler_wait(&slot->ready);
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_scheduler_lock);
  ret = slot->start_result;
  sem_post(&slot->start_consumed);
  if (ret < 0)
    {
      /* Claim reaping under the lock: a concurrent ny_scheduler_stop()
       * that already set "stopping" owns the wait/join/clear sequence,
       * so we must not run it a second time (double join, double
       * sem_destroy).
       */

      if (slot->stopping)
        {
          nxmutex_unlock(&g_scheduler_lock);
          return ret;
        }

      slot->stopping = true;
    }

  nxmutex_unlock(&g_scheduler_lock);
  if (ret < 0)
    {
      ny_scheduler_wait(&slot->stopped);
#ifdef CONFIG_BUILD_KERNEL
      pthread_join(slot->thread, NULL);
#endif
      nxmutex_lock(&g_scheduler_lock);
      ny_scheduler_clear_locked(slot);
      nxmutex_unlock(&g_scheduler_lock);
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_scheduler_dispatch(const char *id, const char *event)
{
  struct ny_scheduler_slot_s *slot;
  uint64_t now;
  int ret;

  if (id == NULL || event == NULL ||
      strlen(event) >= CONFIG_NYABULA_CORE_EVENT_SIZE)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_scheduler_lock);
  slot = ny_scheduler_find_locked(id);
  if (slot == NULL)
    {
      ret = -ENOENT;
    }
  else if (slot->state != NY_PLUGIN_RUNNING || slot->stopping)
    {
      ret = -EPIPE;
    }
  else
    {
      now = ny_scheduler_now_ns();
      if (slot->event_window_ns == 0 ||
          now - slot->event_window_ns >= NY_MINUTE_NS)
        {
          slot->event_window_ns = now;
          slot->event_count = 0;
        }

      if (slot->event_count >= slot->config.events_per_minute)
        {
          ret = -EDQUOT;
        }
      else
        {
          ret = ny_scheduler_enqueue_locked(slot, event);
          if (ret >= 0)
            {
              slot->event_count++;
            }
        }
    }

  nxmutex_unlock(&g_scheduler_lock);
  return ret;
}

int ny_scheduler_complete(const char *id, uint64_t token, int status,
                          const char *payload, size_t length)
{
  struct ny_scheduler_slot_s *slot;
  struct ny_scheduler_completion_s *completion;
  size_t index;

  if (id == NULL || token == 0 || status > 0 ||
      (payload == NULL && length != 0))
    {
      return -EINVAL;
    }

  if (length > CONFIG_NYABULA_CORE_EVENT_SIZE)
    {
      return -EMSGSIZE;
    }

  nxmutex_lock(&g_scheduler_lock);
  slot = ny_scheduler_find_locked(id);
  if (slot == NULL || slot->generation != (uint32_t)(token >> 32))
    {
      nxmutex_unlock(&g_scheduler_lock);
      return -ESTALE;
    }

  if (slot->stopping || !slot->accept_completions)
    {
      nxmutex_unlock(&g_scheduler_lock);
      return -ECANCELED;
    }

  for (index = 0; index < slot->completion_count; index++)
    {
      size_t offset = (slot->completion_head + index) % NY_PLUGIN_MAX_PENDING;
      if (slot->completions[offset].token == token)
        {
          nxmutex_unlock(&g_scheduler_lock);
          return -EALREADY;
        }
    }

  if (slot->completion_count == NY_PLUGIN_MAX_PENDING)
    {
      nxmutex_unlock(&g_scheduler_lock);
      return -EAGAIN;
    }

  index =
      (slot->completion_head + slot->completion_count) % NY_PLUGIN_MAX_PENDING;
  completion = &slot->completions[index];
  completion->token = token;
  completion->status = status;
  completion->length = length;
  if (length != 0)
    {
      memcpy(completion->payload, payload, length);
    }

  slot->completion_count++;
  sem_post(&slot->pending);
  nxmutex_unlock(&g_scheduler_lock);
  return 0;
}

int ny_scheduler_stop(const char *id)
{
  struct ny_scheduler_slot_s *slot;
  bool already_stopped;
  int ret;

  if (id == NULL)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_scheduler_lock);
  slot = ny_scheduler_find_locked(id);
  if (slot == NULL)
    {
      nxmutex_unlock(&g_scheduler_lock);
      return -ENOENT;
    }

  if (slot->stopping)
    {
      nxmutex_unlock(&g_scheduler_lock);
      return -EBUSY;
    }

  already_stopped = slot->task_stopped;
  ret = 0;
  slot->stopping = true;
  slot->accept_completions = false;
  if (!already_stopped)
    {
      sem_post(&slot->pending);
    }

  nxmutex_unlock(&g_scheduler_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!already_stopped)
    {
      ret = ny_scheduler_wait(&slot->stopped);
      if (ret < 0)
        {
          return ret;
        }
    }

#ifdef CONFIG_BUILD_KERNEL
  pthread_join(slot->thread, NULL);
#endif

  ret = ny_scheduler_wait(&slot->start_consumed);
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_scheduler_lock);
  ny_scheduler_clear_locked(slot);
  nxmutex_unlock(&g_scheduler_lock);
  return 0;
}

int ny_scheduler_stop_all(void)
{
  char ids[CONFIG_NYABULA_CORE_MAX_PLUGINS][NY_PLUGIN_ID_SIZE];
  int count = 0;
  int index;
  int ret = 0;

  nxmutex_lock(&g_scheduler_lock);
  for (index = 0; index < CONFIG_NYABULA_CORE_MAX_PLUGINS; index++)
    {
      if (g_scheduler_slots[index].occupied)
        {
          strlcpy(ids[count++], g_scheduler_slots[index].id,
                  NY_PLUGIN_ID_SIZE);
        }
    }

  nxmutex_unlock(&g_scheduler_lock);
  for (index = 0; index < count; index++)
    {
      int stop_ret = ny_scheduler_stop(ids[index]);

      if (stop_ret < 0 && ret == 0)
        {
          ret = stop_ret;
        }
    }

  return ret;
}

void ny_scheduler_list(void)
{
  int index;

  nxmutex_lock(&g_scheduler_lock);
  printf("ID\tSTATE\tCLASS\tPID\tQUEUED\tPATH\n");
  for (index = 0; index < CONFIG_NYABULA_CORE_MAX_PLUGINS; index++)
    {
      struct ny_scheduler_slot_s *slot = &g_scheduler_slots[index];

      if (slot->occupied)
        {
          printf("%s\t%s\t%s\t%d\t%zu\t%s\n", slot->id,
                 ny_scheduler_state_name(slot->state),
                 slot->config.background ? "background" : "foreground",
                 slot->pid, slot->count, slot->path);
        }
    }

  nxmutex_unlock(&g_scheduler_lock);
}
