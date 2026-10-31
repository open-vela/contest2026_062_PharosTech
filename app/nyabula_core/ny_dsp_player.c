/****************************************************************************
 * app/nyabula_core/ny_dsp_player.c
 *
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

#include "ny_dsp_player.h"
#include "ny_audio_stream.h"
#include <errno.h>
#include <fcntl.h>
#include <nuttx/audio/audio.h>
#include <nuttx/mutex.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <system/nxplayer.h>
#include <unistd.h>

#define NY_DSP_PLAYER_HEADER 44
#define NY_DSP_PLAYER_STACK  16384

struct ny_dsp_player_s
{
  mutex_t lock;
  struct ny_audio_stream_s *stream;
  pthread_t thread;
  bool joinable;
  bool stop;
  int fd;
  int state;
  int error;
  unsigned int volume;
  char device[128];
};

static void *ny_dsp_player_worker(void *arg);
static int ny_dsp_player_volume(struct ny_dsp_player_s *player);
static int ny_dsp_player_paused(struct ny_dsp_player_s *player, bool paused);

/****************************************************************************
 * Name: ny_dsp_player_volume
 ****************************************************************************/

static int ny_dsp_player_volume(struct ny_dsp_player_s *player)
{
#ifndef CONFIG_AUDIO_EXCLUDE_VOLUME
  struct audio_caps_desc_s desc;
  int ret;
  int fd = open(player->device, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    {
      return -errno;
    }

  memset(&desc, 0, sizeof(desc));
  desc.caps.ac_len = sizeof(struct audio_caps_s);
  desc.caps.ac_type = AUDIO_TYPE_FEATURE;
  desc.caps.ac_format.hw = AUDIO_FU_VOLUME;
  desc.caps.ac_controls.hw[0] = player->volume;
  ret = ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&desc);
  ret = ret < 0 ? -errno : 0;
  close(fd);
  return ret;
#else
  (void)player;
  return -ENOTSUP;
#endif
}

/****************************************************************************
 * Name: ny_dsp_player_worker
 ****************************************************************************/

static void *ny_dsp_player_worker(void *arg)
{
  struct ny_dsp_player_s *player = arg;
  int16_t samples[512];
  int ret = 0;
  for (;;)
    {
      nxmutex_lock(&player->lock);
      if (player->stop)
        {
          nxmutex_unlock(&player->lock);
          break;
        }

      if (player->state == NXPLAYER_STATE_PAUSED)
        {
          nxmutex_unlock(&player->lock);
          usleep(10000);
          continue;
        }

      ssize_t got = read(player->fd, samples, sizeof(samples));
      if (got <= 0)
        {
          ret = got < 0 ? -errno : ny_audio_stream_drain(player->stream, 500);
          nxmutex_unlock(&player->lock);
          break;
        }

      ssize_t written =
          ny_audio_stream_write(player->stream, samples, got, 100);
      nxmutex_unlock(&player->lock);
      if (written != got)
        {
          ret = written < 0 ? written : -ETIMEDOUT;
          break;
        }
    }

  nxmutex_lock(&player->lock);
  ny_audio_stream_close(player->stream);
  close(player->fd);
  player->stream = NULL;
  player->fd = -1;
  player->error = ret;
  player->state = NXPLAYER_STATE_IDLE;
  nxmutex_unlock(&player->lock);
  return NULL;
}

/****************************************************************************
 * Name: ny_dsp_player_create
 ****************************************************************************/

struct ny_dsp_player_s *ny_dsp_player_create(void)
{
  struct ny_dsp_player_s *player = calloc(1, sizeof(*player));
  if (player != NULL)
    {
      nxmutex_init(&player->lock);
      player->fd = -1;
      player->state = NXPLAYER_STATE_IDLE;
      player->volume = 400;
    }

  return player;
}

/****************************************************************************
 * Name: ny_dsp_player_setdevice
 ****************************************************************************/

int ny_dsp_player_setdevice(struct ny_dsp_player_s *player, const char *path)
{
  if (path == NULL || strlen(path) >= sizeof(player->device))
    {
      return -EINVAL;
    }

  if (access(path, F_OK) < 0)
    {
      return -errno;
    }

  nxmutex_lock(&player->lock);
  if (player->state != NXPLAYER_STATE_IDLE)
    {
      nxmutex_unlock(&player->lock);
      return -EBUSY;
    }

  strlcpy(player->device, path, sizeof(player->device));
  nxmutex_unlock(&player->lock);
  return 0;
}

/****************************************************************************
 * Name: ny_dsp_player_setvolume
 ****************************************************************************/

int ny_dsp_player_setvolume(struct ny_dsp_player_s *player,
                            unsigned int volume)
{
  int ret = 0;
  if (volume > 1000)
    {
      return -EINVAL;
    }

  nxmutex_lock(&player->lock);
  player->volume = volume;
  if (player->device[0] != '\0')
    {
      ret = ny_dsp_player_volume(player);
    }

  nxmutex_unlock(&player->lock);
  return ret;
}

/****************************************************************************
 * Name: ny_dsp_player_playpcmfd
 ****************************************************************************/

int ny_dsp_player_playpcmfd(struct ny_dsp_player_s *player, int fd,
                            uint8_t channels, uint8_t bits, uint32_t rate)
{
  struct ny_audio_stream_config_s config;
  pthread_attr_t attr;
  int ret;
  if (bits != 16)
    {
      return -ENOTSUP;
    }

  if (ny_dsp_player_getstate(player) != NXPLAYER_STATE_IDLE)
    {
      return -EBUSY;
    }

  if (player->joinable)
    {
      pthread_join(player->thread, NULL);
      player->joinable = false;
    }

  if (lseek(fd, NY_DSP_PLAYER_HEADER, SEEK_SET) < 0)
    {
      return -errno;
    }

  memset(&config, 0, sizeof(config));
  config.dsp_source = "file";
  config.path = player->device;
  config.wav = strstr(player->device, "/pcm") != NULL;
  config.rate = rate;
  config.channels = channels;
  config.chunk = 1024;
  config.buffers = 8;
  ret = ny_audio_stream_open(&player->stream, &config);
  if (ret < 0)
    {
      return ret;
    }

  player->fd = fd;
  player->stop = false;
  player->error = 0;
  player->state = NXPLAYER_STATE_PLAYING;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, NY_DSP_PLAYER_STACK);
  ret = pthread_create(&player->thread, &attr, ny_dsp_player_worker, player);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      ny_audio_stream_close(player->stream);
      player->stream = NULL;
      player->fd = -1;
      player->state = NXPLAYER_STATE_IDLE;
      return -ret;
    }

  player->joinable = true;
  return 0;
}

/****************************************************************************
 * Name: ny_dsp_player_getstate / ny_dsp_player_error
 ****************************************************************************/

int ny_dsp_player_getstate(struct ny_dsp_player_s *player)
{
  int state;
  nxmutex_lock(&player->lock);
  state = player->state;
  nxmutex_unlock(&player->lock);
  return state;
}

int ny_dsp_player_error(struct ny_dsp_player_s *player)
{
  int error;
  nxmutex_lock(&player->lock);
  error = player->error;
  nxmutex_unlock(&player->lock);
  return error;
}

/****************************************************************************
 * Name: ny_dsp_player_paused / ny_dsp_player_pause / ny_dsp_player_resume
 ****************************************************************************/

static int ny_dsp_player_paused(struct ny_dsp_player_s *player, bool paused)
{
  int ret;
  nxmutex_lock(&player->lock);
  if (player->stream == NULL || player->state == NXPLAYER_STATE_IDLE)
    {
      nxmutex_unlock(&player->lock);
      return -EALREADY;
    }

  ret = ny_audio_stream_pause(player->stream, paused);
  if (ret == 0)
    {
      player->state = paused ? NXPLAYER_STATE_PAUSED : NXPLAYER_STATE_PLAYING;
    }

  nxmutex_unlock(&player->lock);
  return ret;
}

int ny_dsp_player_pause(struct ny_dsp_player_s *player)
{
  return ny_dsp_player_paused(player, true);
}

int ny_dsp_player_resume(struct ny_dsp_player_s *player)
{
  return ny_dsp_player_paused(player, false);
}

/****************************************************************************
 * Name: ny_dsp_player_stop / ny_dsp_player_release
 ****************************************************************************/

int ny_dsp_player_stop(struct ny_dsp_player_s *player)
{
  nxmutex_lock(&player->lock);
  player->stop = true;
  nxmutex_unlock(&player->lock);
  if (player->joinable)
    {
      pthread_join(player->thread, NULL);
      player->joinable = false;
    }

  return 0;
}

void ny_dsp_player_release(struct ny_dsp_player_s *player)
{
  ny_dsp_player_stop(player);
  nxmutex_destroy(&player->lock);
  free(player);
}
