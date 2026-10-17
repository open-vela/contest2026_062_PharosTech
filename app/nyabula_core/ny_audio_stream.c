/****************************************************************************
 * app/nyabula_core/ny_audio_stream.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "ny_audio_stream.h"

#ifdef CONFIG_NYABULA_CORE_BT

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>

#include "ny_pcm.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_AUDIO_STREAM_BUFFERS_MAX 16
#define NY_AUDIO_STREAM_STOP_MS     600

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum ny_audio_stream_state_e
{
  NY_AUDIO_STREAM_FREE = 0, /* Ours, empty */
  NY_AUDIO_STREAM_QUEUED,   /* With the driver */
  NY_AUDIO_STREAM_READY     /* Capture: ours, holding samples */
};

struct ny_audio_stream_buffer_s
{
  struct ap_buffer_s *apb;
  enum ny_audio_stream_state_e state;
  uint32_t order; /* Capture: arrival order */
};

struct ny_audio_stream_s
{
  struct ny_audio_stream_config_s config;
  struct ny_audio_stream_buffer_s buffers[NY_AUDIO_STREAM_BUFFERS_MAX];
  struct ny_audio_stream_stats_s stats;
  unsigned int count;
  int current;      /* Playback: the chunk being filled, or -1 */
  int fd;
  mqd_t mq;
  char mqname[32];
  bool reserved;
  bool registered;
  bool started;
  bool header_sent;
  bool complete;    /* The driver ended the stream */
  uint32_t arrivals;
  size_t queued;    /* Playback bytes with the driver */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int ny_audio_stream_configure(struct ny_audio_stream_s *stream);
static int ny_audio_stream_enqueue(struct ny_audio_stream_s *stream,
                                   int index);
static int ny_audio_stream_start(struct ny_audio_stream_s *stream);
static int ny_audio_stream_pump(struct ny_audio_stream_s *stream,
                                int timeout_ms);
static int ny_audio_stream_find(struct ny_audio_stream_s *stream,
                                enum ny_audio_stream_state_e state);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_audio_stream_configure
 ****************************************************************************/

static int ny_audio_stream_configure(struct ny_audio_stream_s *stream)
{
  struct audio_caps_desc_s desc;

  memset(&desc, 0, sizeof(desc));
  desc.caps.ac_len = sizeof(struct audio_caps_s);
  desc.caps.ac_type =
      stream->config.capture ? AUDIO_TYPE_INPUT : AUDIO_TYPE_OUTPUT;
  desc.caps.ac_channels = stream->config.channels;
  desc.caps.ac_controls.hw[0] = (uint16_t)stream->config.rate;
  desc.caps.ac_controls.b[2] = 16;
  desc.caps.ac_controls.b[3] = (uint8_t)(stream->config.rate >> 16);
  return ioctl(stream->fd, AUDIOIOC_CONFIGURE,
               (unsigned long)(uintptr_t)&desc) < 0
             ? -errno
             : 0;
}

/****************************************************************************
 * Name: ny_audio_stream_enqueue
 ****************************************************************************/

static int ny_audio_stream_enqueue(struct ny_audio_stream_s *stream,
                                   int index)
{
  struct ny_audio_stream_buffer_s *buffer = &stream->buffers[index];
  struct audio_buf_desc_s desc;

  memset(&desc, 0, sizeof(desc));
  desc.numbytes = buffer->apb->nbytes;
  desc.u.buffer = buffer->apb;
  buffer->apb->curbyte = 0;
  buffer->apb->flags = 0;
  if (ioctl(stream->fd, AUDIOIOC_ENQUEUEBUFFER,
            (unsigned long)(uintptr_t)&desc) < 0)
    {
      return -errno;
    }

  buffer->state = NY_AUDIO_STREAM_QUEUED;
  if (!stream->config.capture)
    {
      stream->queued += buffer->apb->nbytes;
    }

  return 0;
}

/****************************************************************************
 * Name: ny_audio_stream_start
 ****************************************************************************/

static int ny_audio_stream_start(struct ny_audio_stream_s *stream)
{
  if (stream->started)
    {
      return 0;
    }

  if (ioctl(stream->fd, AUDIOIOC_START, 0) < 0)
    {
      return -errno;
    }

  stream->started = true;
  return 0;
}

/****************************************************************************
 * Name: ny_audio_stream_pump
 *
 * Description:
 *   Take what the driver has to say, waiting up to timeout_ms for the first
 *   message only.
 *
 * Returned Value:
 *   Messages handled, or -EPIPE once the stream has been ended.
 *
 ****************************************************************************/

static int ny_audio_stream_pump(struct ny_audio_stream_s *stream,
                                int timeout_ms)
{
  int handled = 0;

  for (; ; )
    {
      struct audio_msg_s msg;
      struct timespec until;
      unsigned int priority;
      unsigned int index;
      ssize_t size;

      clock_gettime(CLOCK_REALTIME, &until);
      if (handled == 0 && timeout_ms > 0)
        {
          until.tv_sec += timeout_ms / 1000;
          until.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
          if (until.tv_nsec >= 1000000000)
            {
              until.tv_sec++;
              until.tv_nsec -= 1000000000;
            }
        }

      size = mq_timedreceive(stream->mq, (char *)&msg, sizeof(msg), &priority,
                             &until);
      if (size < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          break;
        }

      if (size != sizeof(msg))
        {
          continue;
        }

      handled++;
      if (msg.msg_id == AUDIO_MSG_COMPLETE)
        {
          stream->complete = true;
          continue;
        }

      if (msg.msg_id != AUDIO_MSG_DEQUEUE)
        {
          continue;
        }

      for (index = 0; index < stream->count; index++)
        {
          struct ny_audio_stream_buffer_s *buffer = &stream->buffers[index];

          if (buffer->apb != msg.u.ptr ||
              buffer->state != NY_AUDIO_STREAM_QUEUED)
            {
              continue;
            }

          stream->stats.chunks++;
          if (stream->config.capture)
            {
              buffer->state = NY_AUDIO_STREAM_READY;
              buffer->order = stream->arrivals++;
              buffer->apb->curbyte = 0;
            }
          else
            {
              buffer->state = NY_AUDIO_STREAM_FREE;
              stream->queued -= buffer->apb->nbytes;
              buffer->apb->nbytes = 0;
              if (stream->queued == 0 && stream->started)
                {
                  stream->stats.starved++;
                }
            }

          break;
        }
    }

  return stream->complete ? -EPIPE : handled;
}

/****************************************************************************
 * Name: ny_audio_stream_find
 *
 * Description:
 *   The oldest buffer in the given state, or -1.
 *
 ****************************************************************************/

static int ny_audio_stream_find(struct ny_audio_stream_s *stream,
                                enum ny_audio_stream_state_e state)
{
  int found = -1;
  unsigned int index;

  for (index = 0; index < stream->count; index++)
    {
      if (stream->buffers[index].state == state &&
          (int)index != stream->current &&
          (found < 0 || (int32_t)(stream->buffers[index].order -
                                  stream->buffers[found].order) < 0))
        {
          found = (int)index;
        }
    }

  return found;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ny_audio_stream_open
 ****************************************************************************/

int ny_audio_stream_open(struct ny_audio_stream_s **result,
                         const struct ny_audio_stream_config_s *config)
{
  struct ny_audio_stream_s *stream;
  struct ap_buffer_info_s info;
  struct mq_attr attr;
  unsigned int index;
  int ret;

  if (result == NULL || config == NULL || config->path == NULL ||
      config->rate == 0 || config->channels < 1 || config->channels > 2 ||
      config->chunk == 0 || config->chunk % (config->channels * 2) != 0)
    {
      return -EINVAL;
    }

  *result = NULL;
  stream = calloc(1, sizeof(*stream));
  if (stream == NULL)
    {
      return -ENOMEM;
    }

  stream->config = *config;
  stream->current = -1;
  stream->mq = (mqd_t)-1;
  stream->fd = open(config->path, O_RDWR | O_CLOEXEC);
  if (stream->fd < 0)
    {
      ret = -errno;
      goto failed;
    }

  if (ioctl(stream->fd, AUDIOIOC_RESERVE, 0) < 0)
    {
      ret = -errno;
      goto failed;
    }

  stream->reserved = true;
  ret = ny_audio_stream_configure(stream);
  if (ret < 0)
    {
      goto failed;
    }

  /* The upper half only allocates once it has been told how many buffers
   * the driver means to use.
   */

  if (ioctl(stream->fd, AUDIOIOC_GETBUFFERINFO,
            (unsigned long)(uintptr_t)&info) < 0)
    {
      info.nbuffers = CONFIG_AUDIO_NUM_BUFFERS;
      info.buffer_size = CONFIG_AUDIO_BUFFER_NUMBYTES;
    }

  stream->count = config->buffers > 0 ? config->buffers : info.nbuffers;
  if (stream->count < 2)
    {
      stream->count = 2;
    }

  if (stream->count > NY_AUDIO_STREAM_BUFFERS_MAX)
    {
      stream->count = NY_AUDIO_STREAM_BUFFERS_MAX;
    }

  snprintf(stream->mqname, sizeof(stream->mqname), "/tmp/nyas%lx",
           (unsigned long)(uintptr_t)stream);
  memset(&attr, 0, sizeof(attr));
  attr.mq_maxmsg = stream->count + 8;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  stream->mq = mq_open(stream->mqname, O_RDWR | O_CREAT, 0644, &attr);
  if (stream->mq == (mqd_t)-1)
    {
      ret = -errno;
      goto failed;
    }

  if (ioctl(stream->fd, AUDIOIOC_REGISTERMQ, (unsigned long)stream->mq) < 0)
    {
      ret = -errno;
      goto failed;
    }

  stream->registered = true;
  for (index = 0; index < stream->count; index++)
    {
      struct audio_buf_desc_s desc;

      memset(&desc, 0, sizeof(desc));
      desc.numbytes = (apb_samp_t)(config->chunk +
                                   (config->wav ? NY_PCM_WAV_HEADER_BYTES
                                                : 0));
      desc.u.pbuffer = &stream->buffers[index].apb;
      if (ioctl(stream->fd, AUDIOIOC_ALLOCBUFFER,
                (unsigned long)(uintptr_t)&desc) < 0 ||
          stream->buffers[index].apb == NULL)
        {
          ret = errno != 0 ? -errno : -ENOMEM;
          stream->count = index;
          goto failed;
        }

      stream->buffers[index].apb->nbytes = 0;
    }

  if (config->capture)
    {
      for (index = 0; index < stream->count; index++)
        {
          stream->buffers[index].apb->nbytes =
              stream->buffers[index].apb->nmaxbytes;
          ret = ny_audio_stream_enqueue(stream, (int)index);
          if (ret < 0)
            {
              goto failed;
            }
        }

      ret = ny_audio_stream_start(stream);
      if (ret < 0)
        {
          goto failed;
        }
    }

  *result = stream;
  return 0;

failed:
  ny_audio_stream_close(stream);
  return ret;
}

/****************************************************************************
 * Name: ny_audio_stream_write
 ****************************************************************************/

ssize_t ny_audio_stream_write(struct ny_audio_stream_s *stream,
                              const void *pcm, size_t bytes, int timeout_ms)
{
  const uint8_t *source = pcm;
  size_t done = 0;

  if (stream == NULL || pcm == NULL || stream->config.capture)
    {
      return -EINVAL;
    }

  while (done < bytes)
    {
      struct ap_buffer_s *apb;
      size_t target;
      size_t room;
      int ret;

      if (stream->current < 0)
        {
          ret = ny_audio_stream_pump(stream, 0);
          if (ret == -EPIPE)
            {
              return ret;
            }

          stream->current = ny_audio_stream_find(stream,
                                                 NY_AUDIO_STREAM_FREE);
          if (stream->current < 0)
            {
              /* Everything is queued: this wait is the pacing. */

              ret = ny_audio_stream_pump(stream, timeout_ms);
              if (ret == -EPIPE)
                {
                  return ret;
                }

              stream->current = ny_audio_stream_find(stream,
                                                     NY_AUDIO_STREAM_FREE);
              if (stream->current < 0)
                {
                  stream->stats.dropped++;
                  break;
                }
            }

          apb = stream->buffers[stream->current].apb;
          apb->nbytes = 0;
          if (stream->config.wav && !stream->header_sent)
            {
              ny_pcm_wav_header(apb->samp, stream->config.rate,
                                stream->config.channels);
              apb->nbytes = NY_PCM_WAV_HEADER_BYTES;
            }
        }

      apb = stream->buffers[stream->current].apb;
      target = stream->config.chunk;
      if (stream->config.wav && !stream->header_sent)
        {
          target += NY_PCM_WAV_HEADER_BYTES;
        }

      room = target - apb->nbytes;
      if (room > bytes - done)
        {
          room = bytes - done;
        }

      memcpy(apb->samp + apb->nbytes, source + done, room);
      apb->nbytes += (apb_samp_t)room;
      done += room;
      if (apb->nbytes >= target)
        {
          int index = stream->current;

          stream->current = -1;
          ret = ny_audio_stream_enqueue(stream, index);
          if (ret < 0)
            {
              stream->buffers[index].apb->nbytes = 0;
              return ret;
            }

          stream->header_sent = true;
          ret = ny_audio_stream_start(stream);
          if (ret < 0)
            {
              return ret;
            }
        }
    }

  return (ssize_t)done;
}

/****************************************************************************
 * Name: ny_audio_stream_read
 ****************************************************************************/

ssize_t ny_audio_stream_read(struct ny_audio_stream_s *stream, void *pcm,
                             size_t bytes, int timeout_ms)
{
  uint8_t *target = pcm;
  size_t done = 0;

  if (stream == NULL || pcm == NULL || !stream->config.capture)
    {
      return -EINVAL;
    }

  while (done < bytes)
    {
      struct ap_buffer_s *apb;
      size_t available;
      int index;
      int ret;

      index = ny_audio_stream_find(stream, NY_AUDIO_STREAM_READY);
      if (index < 0)
        {
          ret = ny_audio_stream_pump(stream, timeout_ms);
          if (ret == -EPIPE)
            {
              return done > 0 ? (ssize_t)done : ret;
            }

          index = ny_audio_stream_find(stream, NY_AUDIO_STREAM_READY);
          if (index < 0)
            {
              break;
            }
        }

      apb = stream->buffers[index].apb;
      available = apb->nbytes > apb->curbyte ? apb->nbytes - apb->curbyte : 0;
      if (available > bytes - done)
        {
          available = bytes - done;
        }

      memcpy(target + done, apb->samp + apb->curbyte, available);
      apb->curbyte += (apb_samp_t)available;
      done += available;
      if (apb->curbyte >= apb->nbytes)
        {
          apb->nbytes = apb->nmaxbytes;
          ret = ny_audio_stream_enqueue(stream, index);
          if (ret < 0)
            {
              stream->buffers[index].state = NY_AUDIO_STREAM_FREE;
              return done > 0 ? (ssize_t)done : ret;
            }
        }
    }

  return (ssize_t)done;
}

/****************************************************************************
 * Name: ny_audio_stream_queued
 ****************************************************************************/

size_t ny_audio_stream_queued(struct ny_audio_stream_s *stream)
{
  if (stream == NULL || stream->config.capture)
    {
      return 0;
    }

  ny_audio_stream_pump(stream, 0);
  return stream->queued;
}

/****************************************************************************
 * Name: ny_audio_stream_stats
 ****************************************************************************/

void ny_audio_stream_stats(struct ny_audio_stream_s *stream,
                           struct ny_audio_stream_stats_s *stats)
{
  if (stream == NULL)
    {
      memset(stats, 0, sizeof(*stats));
    }
  else
    {
      *stats = stream->stats;
    }
}

/****************************************************************************
 * Name: ny_audio_stream_close
 ****************************************************************************/

void ny_audio_stream_close(struct ny_audio_stream_s *stream)
{
  unsigned int index;

  if (stream == NULL)
    {
      return;
    }

  if (stream->started)
    {
      struct timespec begin;

      /* A stopped driver hands every chunk back and then says so; nothing
       * may be freed before that.
       */

      ioctl(stream->fd, AUDIOIOC_STOP, 0);
      clock_gettime(CLOCK_MONOTONIC, &begin);
      while (!stream->complete)
        {
          struct timespec now;

          ny_audio_stream_pump(stream, 50);
          clock_gettime(CLOCK_MONOTONIC, &now);
          if ((now.tv_sec - begin.tv_sec) * 1000 +
                  (now.tv_nsec - begin.tv_nsec) / 1000000 >
              NY_AUDIO_STREAM_STOP_MS)
            {
              break;
            }
        }
    }

  for (index = 0; index < stream->count; index++)
    {
      struct ny_audio_stream_buffer_s *buffer = &stream->buffers[index];
      struct audio_buf_desc_s desc;

      if (buffer->apb == NULL)
        {
          continue;
        }

      if (buffer->state == NY_AUDIO_STREAM_QUEUED && !stream->complete)
        {
          /* Still the driver's: leaking it is the lesser evil. */

          syslog(LOG_WARNING, "nybt: audio chunk left with %s\n",
                 stream->config.path);
          continue;
        }

      memset(&desc, 0, sizeof(desc));
      desc.u.buffer = buffer->apb;
      ioctl(stream->fd, AUDIOIOC_FREEBUFFER, (unsigned long)(uintptr_t)&desc);
    }

  if (stream->registered)
    {
      ioctl(stream->fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)stream->mq);
    }

  if (stream->reserved)
    {
      ioctl(stream->fd, AUDIOIOC_RELEASE, 0);
    }

  if (stream->fd >= 0)
    {
      close(stream->fd);
    }

  if (stream->mq != (mqd_t)-1)
    {
      mq_close(stream->mq);
      mq_unlink(stream->mqname);
    }

  free(stream);
}

#endif /* CONFIG_NYABULA_CORE_BT */
