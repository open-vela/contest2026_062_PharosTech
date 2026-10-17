/****************************************************************************
 * app/nyabula_core/ny_voice_audio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_NYABULA_CORE_VOICE

#include <nuttx/audio/audio.h>

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

#include "ny_voice_audio.h"
#include "ny_voice_dsp.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_VOICE_AUDIO_BUFFERS 8
#define NY_VOICE_AUDIO_STOP_MS 600

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum ny_voice_audio_state_e
{
  NY_VOICE_AUDIO_FREE = 0, /* Ours, empty.                      */
  NY_VOICE_AUDIO_QUEUED,   /* With the driver.                  */
  NY_VOICE_AUDIO_READY     /* Capture: ours, holding samples.   */
};

struct ny_voice_audio_buffer_s
{
  struct ap_buffer_s *apb;
  enum ny_voice_audio_state_e state;
  uint32_t order; /* Capture: arrival order. */
};

struct ny_voice_audio_s
{
  struct ny_voice_audio_config_s config;
  struct ny_voice_audio_buffer_s buffers[NY_VOICE_AUDIO_BUFFERS];
  unsigned int count;
  int current;        /* Playback: the chunk being filled, or -1. */
  int fd;
  mqd_t mq;
  char mqname[32];
  bool reserved;
  bool registered;
  bool started;
  bool header_sent;
  bool complete;      /* The driver ended the stream. */
  uint32_t arrivals;
  size_t queued;      /* Playback bytes with the driver. */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int ny_voice_audio_configure(struct ny_voice_audio_s *audio);
static int ny_voice_audio_enqueue(struct ny_voice_audio_s *audio, int index);
static int ny_voice_audio_start(struct ny_voice_audio_s *audio);
static int ny_voice_audio_pump(struct ny_voice_audio_s *audio,
                               int timeout_ms);
static int ny_voice_audio_find(struct ny_voice_audio_s *audio,
                               enum ny_voice_audio_state_e state);
static size_t ny_voice_audio_target(const struct ny_voice_audio_s *audio);
static int ny_voice_audio_submit(struct ny_voice_audio_s *audio);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int ny_voice_audio_configure(struct ny_voice_audio_s *audio)
{
  struct audio_caps_desc_s desc;

  memset(&desc, 0, sizeof(desc));
  desc.caps.ac_len = sizeof(struct audio_caps_s);
  desc.caps.ac_type =
      audio->config.capture ? AUDIO_TYPE_INPUT : AUDIO_TYPE_OUTPUT;
  desc.caps.ac_channels = audio->config.channels;
  desc.caps.ac_controls.hw[0] = (uint16_t)audio->config.rate;
  desc.caps.ac_controls.b[2] = 16;
  desc.caps.ac_controls.b[3] = (uint8_t)(audio->config.rate >> 16);
  return ioctl(audio->fd, AUDIOIOC_CONFIGURE,
               (unsigned long)(uintptr_t)&desc) < 0
             ? -errno
             : 0;
}

static int ny_voice_audio_enqueue(struct ny_voice_audio_s *audio, int index)
{
  struct ny_voice_audio_buffer_s *buffer = &audio->buffers[index];
  struct audio_buf_desc_s desc;

  memset(&desc, 0, sizeof(desc));
  desc.numbytes = buffer->apb->nbytes;
  desc.u.buffer = buffer->apb;
  buffer->apb->curbyte = 0;
  buffer->apb->flags = 0;
  if (ioctl(audio->fd, AUDIOIOC_ENQUEUEBUFFER,
            (unsigned long)(uintptr_t)&desc) < 0)
    {
      return -errno;
    }

  buffer->state = NY_VOICE_AUDIO_QUEUED;
  if (!audio->config.capture)
    {
      audio->queued += buffer->apb->nbytes;
    }

  return 0;
}

static int ny_voice_audio_start(struct ny_voice_audio_s *audio)
{
  if (audio->started)
    {
      return 0;
    }

  if (ioctl(audio->fd, AUDIOIOC_START, 0) < 0)
    {
      return -errno;
    }

  audio->started = true;
  return 0;
}

/****************************************************************************
 * Name: ny_voice_audio_pump
 *
 * Description:
 *   Take what the driver has to say, waiting up to timeout_ms for the first
 *   message only.  Returns the messages handled, or -EPIPE once the stream
 *   has been ended by the driver.
 *
 ****************************************************************************/

static int ny_voice_audio_pump(struct ny_voice_audio_s *audio, int timeout_ms)
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

      size = mq_timedreceive(audio->mq, (char *)&msg, sizeof(msg), &priority,
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
          audio->complete = true;
          continue;
        }

      if (msg.msg_id != AUDIO_MSG_DEQUEUE)
        {
          continue;
        }

      for (index = 0; index < audio->count; index++)
        {
          struct ny_voice_audio_buffer_s *buffer = &audio->buffers[index];

          if (buffer->apb != msg.u.ptr ||
              buffer->state != NY_VOICE_AUDIO_QUEUED)
            {
              continue;
            }

          if (audio->config.capture)
            {
              buffer->state = NY_VOICE_AUDIO_READY;
              buffer->order = audio->arrivals++;
              buffer->apb->curbyte = 0;
            }
          else
            {
              buffer->state = NY_VOICE_AUDIO_FREE;
              audio->queued -= buffer->apb->nbytes;
              buffer->apb->nbytes = 0;
            }

          break;
        }
    }

  return audio->complete ? -EPIPE : handled;
}

/****************************************************************************
 * Name: ny_voice_audio_find
 *
 * Description:
 *   The oldest buffer in the given state, or -1.
 *
 ****************************************************************************/

static int ny_voice_audio_find(struct ny_voice_audio_s *audio,
                               enum ny_voice_audio_state_e state)
{
  int found = -1;
  unsigned int index;

  for (index = 0; index < audio->count; index++)
    {
      if (audio->buffers[index].state == state &&
          (int)index != audio->current &&
          (found < 0 || (int32_t)(audio->buffers[index].order -
                                  audio->buffers[found].order) < 0))
        {
          found = (int)index;
        }
    }

  return found;
}

static size_t ny_voice_audio_target(const struct ny_voice_audio_s *audio)
{
  /* The first chunk also carries the WAV header. */

  return audio->config.chunk +
         (audio->config.wav && !audio->header_sent ? NY_VOICE_WAV_HEADER : 0);
}

/****************************************************************************
 * Name: ny_voice_audio_submit
 *
 * Description:
 *   Hand the chunk being filled to the driver.
 *
 ****************************************************************************/

static int ny_voice_audio_submit(struct ny_voice_audio_s *audio)
{
  int index = audio->current;
  int ret;

  audio->current = -1;
  ret = ny_voice_audio_enqueue(audio, index);
  if (ret < 0)
    {
      audio->buffers[index].apb->nbytes = 0;
      return ret;
    }

  audio->header_sent = true;
  return ny_voice_audio_start(audio);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_voice_audio_open(struct ny_voice_audio_s **result,
                        const struct ny_voice_audio_config_s *config)
{
  struct ny_voice_audio_s *audio;
  struct ap_buffer_info_s info;
  struct mq_attr attr;
  unsigned int index;
  int ret;

  if (result == NULL || config == NULL || config->path == NULL ||
      config->rate == 0 || config->channels < 1 || config->channels > 2 ||
      config->chunk == 0 || config->chunk % (config->channels * 2U) != 0)
    {
      return -EINVAL;
    }

  *result = NULL;
  audio = calloc(1, sizeof(*audio));
  if (audio == NULL)
    {
      return -ENOMEM;
    }

  audio->config = *config;
  audio->current = -1;
  audio->mq = (mqd_t)-1;
  audio->fd = open(config->path, O_RDWR | O_CLOEXEC);
  if (audio->fd < 0)
    {
      ret = -errno;
      goto failed;
    }

  if (ioctl(audio->fd, AUDIOIOC_RESERVE, 0) < 0)
    {
      ret = -errno;
      goto failed;
    }

  audio->reserved = true;
  ret = ny_voice_audio_configure(audio);
  if (ret < 0)
    {
      goto failed;
    }

  /* The upper half only allocates once it has been asked how many buffers
   * the driver means to use; the answer itself is not needed.
   */

  ioctl(audio->fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)(uintptr_t)&info);
  audio->count = NY_VOICE_AUDIO_BUFFERS;

  snprintf(audio->mqname, sizeof(audio->mqname), "/tmp/nyvo%lx",
           (unsigned long)(uintptr_t)audio);
  memset(&attr, 0, sizeof(attr));
  attr.mq_maxmsg = NY_VOICE_AUDIO_BUFFERS + 8;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  audio->mq = mq_open(audio->mqname, O_RDWR | O_CREAT, 0644, &attr);
  if (audio->mq == (mqd_t)-1)
    {
      ret = -errno;
      goto failed;
    }

  if (ioctl(audio->fd, AUDIOIOC_REGISTERMQ, (unsigned long)audio->mq) < 0)
    {
      ret = -errno;
      goto failed;
    }

  audio->registered = true;
  for (index = 0; index < audio->count; index++)
    {
      struct audio_buf_desc_s desc;

      memset(&desc, 0, sizeof(desc));
      desc.numbytes = (apb_samp_t)(config->chunk +
                                   (config->wav ? NY_VOICE_WAV_HEADER : 0));
      desc.u.pbuffer = &audio->buffers[index].apb;
      if (ioctl(audio->fd, AUDIOIOC_ALLOCBUFFER,
                (unsigned long)(uintptr_t)&desc) < 0 ||
          audio->buffers[index].apb == NULL)
        {
          ret = errno != 0 ? -errno : -ENOMEM;
          audio->count = index;
          goto failed;
        }

      audio->buffers[index].apb->nbytes = 0;
    }

  if (config->capture)
    {
      for (index = 0; index < audio->count; index++)
        {
          audio->buffers[index].apb->nbytes =
              audio->buffers[index].apb->nmaxbytes;
          ret = ny_voice_audio_enqueue(audio, (int)index);
          if (ret < 0)
            {
              goto failed;
            }
        }

      ret = ny_voice_audio_start(audio);
      if (ret < 0)
        {
          goto failed;
        }
    }

  *result = audio;
  return 0;

failed:
  ny_voice_audio_close(audio);
  return ret;
}

ssize_t ny_voice_audio_read(struct ny_voice_audio_s *audio, void *pcm,
                            size_t bytes, int timeout_ms)
{
  uint8_t *target = pcm;
  size_t done = 0;

  if (audio == NULL || pcm == NULL || !audio->config.capture)
    {
      return -EINVAL;
    }

  while (done < bytes)
    {
      struct ap_buffer_s *apb;
      size_t available;
      int index;
      int ret;

      index = ny_voice_audio_find(audio, NY_VOICE_AUDIO_READY);
      if (index < 0)
        {
          ret = ny_voice_audio_pump(audio, timeout_ms);
          if (ret == -EPIPE)
            {
              return done > 0 ? (ssize_t)done : ret;
            }

          index = ny_voice_audio_find(audio, NY_VOICE_AUDIO_READY);
          if (index < 0)
            {
              break;
            }
        }

      apb = audio->buffers[index].apb;
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
          ret = ny_voice_audio_enqueue(audio, index);
          if (ret < 0)
            {
              audio->buffers[index].state = NY_VOICE_AUDIO_FREE;
              return done > 0 ? (ssize_t)done : ret;
            }
        }
    }

  return (ssize_t)done;
}

ssize_t ny_voice_audio_write(struct ny_voice_audio_s *audio, const void *pcm,
                             size_t bytes, int timeout_ms)
{
  const uint8_t *source = pcm;
  size_t done = 0;

  if (audio == NULL || pcm == NULL || audio->config.capture)
    {
      return -EINVAL;
    }

  while (done < bytes)
    {
      struct ap_buffer_s *apb;
      size_t room;
      int ret;

      if (audio->current < 0)
        {
          ret = ny_voice_audio_pump(audio, 0);
          if (ret == -EPIPE)
            {
              return ret;
            }

          audio->current = ny_voice_audio_find(audio, NY_VOICE_AUDIO_FREE);
          if (audio->current < 0)
            {
              /* Everything is queued: this wait is the pacing. */

              ret = ny_voice_audio_pump(audio, timeout_ms);
              if (ret == -EPIPE)
                {
                  return ret;
                }

              audio->current = ny_voice_audio_find(audio,
                                                   NY_VOICE_AUDIO_FREE);
              if (audio->current < 0)
                {
                  break;
                }
            }

          apb = audio->buffers[audio->current].apb;
          apb->nbytes = 0;
          if (audio->config.wav && !audio->header_sent)
            {
              ny_voice_wav_header(apb->samp, audio->config.rate,
                                  audio->config.channels, 0);
              apb->nbytes = NY_VOICE_WAV_HEADER;
            }
        }

      apb = audio->buffers[audio->current].apb;
      room = ny_voice_audio_target(audio) - apb->nbytes;
      if (room > bytes - done)
        {
          room = bytes - done;
        }

      memcpy(apb->samp + apb->nbytes, source + done, room);
      apb->nbytes += (apb_samp_t)room;
      done += room;
      if (apb->nbytes >= ny_voice_audio_target(audio))
        {
          ret = ny_voice_audio_submit(audio);
          if (ret < 0)
            {
              return ret;
            }
        }
    }

  return (ssize_t)done;
}

size_t ny_voice_audio_queued(struct ny_voice_audio_s *audio)
{
  if (audio == NULL || audio->config.capture)
    {
      return 0;
    }

  ny_voice_audio_pump(audio, 0);
  return audio->queued;
}

int ny_voice_audio_drain(struct ny_voice_audio_s *audio, int timeout_ms)
{
  struct timespec begin;
  int ret;

  if (audio == NULL || audio->config.capture)
    {
      return -EINVAL;
    }

  if (audio->current >= 0)
    {
      struct ap_buffer_s *apb = audio->buffers[audio->current].apb;
      size_t target = ny_voice_audio_target(audio);

      /* A whole chunk, the rest of it silence: the driver is not trusted
       * with a short buffer in the middle of what it sees as one stream.
       */

      memset(apb->samp + apb->nbytes, 0, target - apb->nbytes);
      apb->nbytes = (apb_samp_t)target;
      ret = ny_voice_audio_submit(audio);
      if (ret < 0)
        {
          return ret;
        }
    }

  clock_gettime(CLOCK_MONOTONIC, &begin);
  while (audio->queued != 0)
    {
      struct timespec now;

      if (ny_voice_audio_pump(audio, 50) == -EPIPE)
        {
          return 0;
        }

      clock_gettime(CLOCK_MONOTONIC, &now);
      if ((now.tv_sec - begin.tv_sec) * 1000 +
              (now.tv_nsec - begin.tv_nsec) / 1000000 > timeout_ms)
        {
          return -ETIMEDOUT;
        }
    }

  return 0;
}

void ny_voice_audio_close(struct ny_voice_audio_s *audio)
{
  unsigned int index;

  if (audio == NULL)
    {
      return;
    }

  if (audio->started)
    {
      struct timespec begin;

      /* A stopped driver hands every chunk back and then says so; nothing
       * may be freed before that.
       */

      ioctl(audio->fd, AUDIOIOC_STOP, 0);
      clock_gettime(CLOCK_MONOTONIC, &begin);
      while (!audio->complete)
        {
          struct timespec now;

          ny_voice_audio_pump(audio, 50);
          clock_gettime(CLOCK_MONOTONIC, &now);
          if ((now.tv_sec - begin.tv_sec) * 1000 +
                  (now.tv_nsec - begin.tv_nsec) / 1000000 >
              NY_VOICE_AUDIO_STOP_MS)
            {
              break;
            }
        }
    }

  for (index = 0; index < audio->count; index++)
    {
      struct ny_voice_audio_buffer_s *buffer = &audio->buffers[index];
      struct audio_buf_desc_s desc;

      if (buffer->apb == NULL)
        {
          continue;
        }

      if (buffer->state == NY_VOICE_AUDIO_QUEUED && !audio->complete)
        {
          /* Still the driver's: leaking it is the lesser evil. */

          syslog(LOG_WARNING, "nyvoice: audio chunk left with %s\n",
                 audio->config.path);
          continue;
        }

      memset(&desc, 0, sizeof(desc));
      desc.u.buffer = buffer->apb;
      ioctl(audio->fd, AUDIOIOC_FREEBUFFER, (unsigned long)(uintptr_t)&desc);
    }

  if (audio->registered)
    {
      ioctl(audio->fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)audio->mq);
    }

  if (audio->reserved)
    {
      ioctl(audio->fd, AUDIOIOC_RELEASE, 0);
    }

  if (audio->fd >= 0)
    {
      close(audio->fd);
    }

  if (audio->mq != (mqd_t)-1)
    {
      mq_close(audio->mq);
      mq_unlink(audio->mqname);
    }

  free(audio);
}

#endif /* CONFIG_NYABULA_CORE_VOICE */
