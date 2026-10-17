/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_product_device.c
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

#include "ny_product.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <net/if.h>
#include <nuttx/clock.h>
#include <nuttx/config.h>
#include <nuttx/wireless/wireless.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#ifdef CONFIG_AUDIO
#include <nuttx/audio/audio.h>
#endif

#define NY_DEVICE_ITEMS 16

static cJSON *ny_product_device_cpu(void);
static cJSON *ny_product_device_storage(void);
static cJSON *ny_product_device_network(void);
static cJSON *ny_product_device_audio(void);

/****************************************************************************
 * Name: ny_product_device_cpu
 ****************************************************************************/

static cJSON *ny_product_device_cpu(void)
{
  cJSON *cpu = cJSON_CreateObject();
  if (cpu == NULL)
    return NULL;
#if !defined(CONFIG_SCHED_CPULOAD_NONE) && !defined(CONFIG_SMP) && \
    !defined(CONFIG_SCHED_TICKLESS)
  struct cpuload_s load;
  if (clock_cpuload(0, &load) == 0 && load.total > 0 &&
      load.active <= load.total)
    {
      cJSON_AddBoolToObject(cpu, "available", true);
      cJSON_AddNumberToObject(cpu, "percent",
                              100.0 * (load.total - load.active) / load.total);
      cJSON_AddNumberToObject(cpu, "samples", load.total);
      cJSON_AddStringToObject(cpu, "source", "scheduler_sampling");
      return cpu;
    }
#endif
  cJSON_AddBoolToObject(cpu, "available", false);
  cJSON_AddStringToObject(cpu, "reason", "sampler_unavailable");
  return cpu;
}

/****************************************************************************
 * Name: ny_product_device_storage
 ****************************************************************************/

static cJSON *ny_product_device_storage(void)
{
  static const char *const paths[] = {
    "/data",
    "/tmp",
#ifdef CONFIG_ARCH_SIM
    "/host",
#endif
  };
  cJSON *items = cJSON_CreateArray();
  if (items == NULL)
    return NULL;
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
    {
      struct statvfs info;
      cJSON *item = cJSON_CreateObject();
      if (item == NULL)
        {
          cJSON_Delete(items);
          return NULL;
        }
      cJSON_AddStringToObject(item, "path", paths[i]);
      int ret = statvfs(paths[i], &info);
      cJSON_AddBoolToObject(item, "available", ret == 0);
      if (ret == 0)
        {
          double unit = info.f_frsize ? info.f_frsize : info.f_bsize;
          cJSON_AddNumberToObject(item, "totalBytes", info.f_blocks * unit);
          cJSON_AddNumberToObject(item, "freeBytes", info.f_bavail * unit);
        }
      else
        cJSON_AddNumberToObject(item, "error", -errno);
      cJSON_AddItemToArray(items, item);
    }
  return items;
}

/****************************************************************************
 * Name: ny_product_device_network
 ****************************************************************************/

static cJSON *ny_product_device_network(void)
{
  cJSON *network = cJSON_CreateObject();
  cJSON *items = cJSON_AddArrayToObject(network, "interfaces");
  if (network == NULL || items == NULL)
    {
      cJSON_Delete(network);
      return NULL;
    }
#if defined(CONFIG_NET) && defined(CONFIG_NETDEV_IFINDEX)
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct if_nameindex *names = fd >= 0 ? if_nameindex() : NULL;
  cJSON_AddBoolToObject(network, "available", names != NULL);
  if (names == NULL)
    {
      cJSON_AddNumberToObject(network, "error", -errno);
      if (fd >= 0)
        close(fd);
      return network;
    }
  size_t i;
  for (i = 0; names[i].if_name != NULL && i < NY_DEVICE_ITEMS; i++)
    {
      cJSON *item = cJSON_CreateObject();
      if (item == NULL)
        break;
      cJSON_AddStringToObject(item, "name", names[i].if_name);
      cJSON_AddNumberToObject(item, "index", names[i].if_index);
      struct ifreq request = { 0 };
      snprintf(request.ifr_name, sizeof(request.ifr_name), "%s",
               names[i].if_name);
      if (ioctl(fd, SIOCGIFFLAGS, (unsigned long)(uintptr_t)&request) == 0)
        {
          cJSON_AddBoolToObject(item, "up", (request.ifr_flags & IFF_UP) != 0);
          cJSON_AddBoolToObject(item, "loopback",
                                (request.ifr_flags & IFF_LOOPBACK) != 0);
        }
      if (ioctl(fd, SIOCGIFADDR, (unsigned long)(uintptr_t)&request) == 0 &&
          request.ifr_addr.sa_family == AF_INET)
        {
          char address[INET_ADDRSTRLEN];
          struct sockaddr_in *ipv4 = (struct sockaddr_in *)&request.ifr_addr;
          if (inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address)))
            cJSON_AddStringToObject(item, "ipv4", address);
        }
      struct iwreq wireless = { 0 };
      char ssid[IW_ESSID_MAX_SIZE + 1] = { 0 };
      snprintf(wireless.ifr_name, sizeof(wireless.ifr_name), "%s",
               names[i].if_name);
      wireless.u.essid.pointer = ssid;
      wireless.u.essid.length = sizeof(ssid) - 1;
      if (ioctl(fd, SIOCGIWESSID, (unsigned long)(uintptr_t)&wireless) == 0)
        cJSON_AddStringToObject(item, "ssid", ssid);
      cJSON_AddItemToArray(items, item);
    }
  cJSON_AddBoolToObject(network, "truncated", names[i].if_name != NULL);
  if_freenameindex(names);
  close(fd);
#else
  cJSON_AddBoolToObject(network, "available", false);
  cJSON_AddNumberToObject(network, "error", -ENOSYS);
#endif
  return network;
}

/****************************************************************************
 * Name: ny_product_device_audio
 ****************************************************************************/

static cJSON *ny_product_device_audio(void)
{
  cJSON *items = cJSON_CreateArray();
  if (items == NULL)
    return NULL;
  DIR *directory = opendir("/dev/audio");
  if (directory == NULL)
    return items;
  struct dirent *entry;
  size_t count = 0;
  while ((entry = readdir(directory)) != NULL && count < NY_DEVICE_ITEMS)
    {
      if (entry->d_name[0] == '.')
        continue;
      char path[128];
      if (snprintf(path, sizeof(path), "/dev/audio/%s", entry->d_name) >=
          sizeof(path))
        continue;
      cJSON *item = cJSON_CreateObject();
      if (item == NULL)
        break;
      cJSON_AddStringToObject(item, "path", path);
#ifdef CONFIG_AUDIO
      int fd = open(path, O_RDONLY);
      struct audio_caps_s caps = { .ac_len = sizeof(caps),
                                   .ac_type = AUDIO_TYPE_QUERY };
      int ret = fd >= 0 ? ioctl(fd, AUDIOIOC_GETCAPS,
                                (unsigned long)(uintptr_t)&caps)
                        : -1;
      cJSON_AddBoolToObject(item, "available", ret >= 0);
      if (ret >= 0)
        {
          cJSON_AddBoolToObject(item, "output",
                                (caps.ac_controls.b[0] & AUDIO_TYPE_OUTPUT) !=
                                    0);
          cJSON_AddBoolToObject(
              item, "input", (caps.ac_controls.b[0] & AUDIO_TYPE_INPUT) != 0);
          cJSON_AddNumberToObject(item, "formats", caps.ac_format.hw);
        }
      else
        cJSON_AddNumberToObject(item, "error", -errno);
      if (fd >= 0)
        close(fd);
#else
      cJSON_AddBoolToObject(item, "available", false);
#endif
      cJSON_AddItemToArray(items, item);
      count++;
    }
  closedir(directory);
  return items;
}

/****************************************************************************
 * Name: ny_product_device_request
 ****************************************************************************/

int ny_product_device_request(const struct ny_product_caller_s *caller,
                              const char *topic, const cJSON *data,
                              cJSON **result)
{
  (void)data;
  if (strcmp(topic, "device.status"))
    return -ENOSYS;
  if (caller->role != NY_PRODUCT_OWNER)
    return -EACCES;
  struct utsname system;
  struct mallinfo memory = mallinfo();
  if (uname(&system) < 0)
    return -errno;
  cJSON *root = cJSON_CreateObject();
  cJSON *heap = cJSON_AddObjectToObject(root, "memory");
  if (root == NULL || heap == NULL)
    {
      cJSON_Delete(root);
      return -ENOMEM;
    }
  cJSON_AddStringToObject(root, "os", system.sysname);
  cJSON_AddStringToObject(root, "arch", system.machine);
#ifdef CONFIG_ARCH_SIM
  cJSON_AddBoolToObject(root, "simulator", true);
#else
  cJSON_AddBoolToObject(root, "simulator", false);
#endif
  cJSON_AddNumberToObject(root, "sampledAtMs", ny_product_time_ms(true));
  cJSON_AddNumberToObject(root, "uptimeMs", ny_product_time_ms(true));
  cJSON_AddNumberToObject(heap, "totalBytes", memory.arena);
  cJSON_AddNumberToObject(heap, "usedBytes", memory.uordblks);
  cJSON_AddNumberToObject(heap, "freeBytes", memory.fordblks);
  cJSON *cpu = ny_product_device_cpu();
  cJSON *storage = ny_product_device_storage();
  cJSON *network = ny_product_device_network();
  cJSON *audio = ny_product_device_audio();
  if (!cpu || !storage || !network || !audio)
    {
      cJSON_Delete(root);
      cJSON_Delete(cpu);
      cJSON_Delete(storage);
      cJSON_Delete(network);
      cJSON_Delete(audio);
      return -ENOMEM;
    }
  cJSON_AddItemToObject(root, "cpu", cpu);
  cJSON_AddItemToObject(root, "storage", storage);
  cJSON_AddItemToObject(root, "network", network);
  cJSON_AddItemToObject(root, "audioDevices", audio);
  *result = root;
  return 0;
}
