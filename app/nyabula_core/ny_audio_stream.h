/****************************************************************************
 * app/nyabula_core/ny_audio_stream.h
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

#ifndef __NYABULA_CORE_NY_AUDIO_STREAM_H
#define __NYABULA_CORE_NY_AUDIO_STREAM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* A live 16 bit PCM stream on a NuttX audio node: samples that are produced
 * while they play, which the file player cannot do (it ends a stream at the
 * first short read).  One thread per stream; nothing here is locked.
 */

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct ny_audio_stream_s; /* Opaque */

struct ny_audio_stream_config_s
{
  const char  *path;     /* /dev/audio/... */
  bool         capture;
  bool         wav;      /* Playback through the PCM decoder node: it takes
                          * the stream format from a WAV header, so one is
                          * sent ahead of the samples.
                          */
  uint32_t     rate;
  uint8_t      channels;
  size_t       chunk;    /* Bytes handed to the driver at a time: the unit of
                          * latency in both directions.
                          */
  unsigned int buffers;  /* Chunks that may be queued; 0 takes the driver's
                          * own count.
                          */
};

struct ny_audio_stream_stats_s
{
  uint32_t chunks;   /* Chunks through the driver */
  uint32_t starved;  /* Playback: times every chunk had been played before
                      * the next one was written.
                      */
  uint32_t dropped;  /* Capture: chunks overwritten unread.  Playback: writes
                      * refused because nothing was free in time.
                      */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ny_audio_stream_open
 *
 * Description:
 *   Reserve and configure the node.  Capture starts at once; playback starts
 *   with the first full chunk.
 *
 * Returned Value:
 *   Zero, or a negated errno value: -EBUSY when the node is reserved by
 *   another player or when the codec's other direction runs a different
 *   format.
 *
 ****************************************************************************/

int ny_audio_stream_open(struct ny_audio_stream_s **stream,
                         const struct ny_audio_stream_config_s *config);

/****************************************************************************
 * Name: ny_audio_stream_write
 *
 * Description:
 *   Queue samples for playback, waiting up to timeout_ms for the driver to
 *   return a chunk when all of them are queued.  That wait is what paces a
 *   producer to the codec clock.
 *
 * Returned Value:
 *   Bytes taken (all of them, or fewer on a timeout), or a negated errno
 *   value; -EPIPE once the driver has ended the stream.
 *
 ****************************************************************************/

ssize_t ny_audio_stream_write(struct ny_audio_stream_s *stream,
                              const void *pcm, size_t bytes, int timeout_ms);

/****************************************************************************
 * Name: ny_audio_stream_read
 *
 * Description:
 *   Take captured samples, waiting up to timeout_ms for them.
 *
 * Returned Value:
 *   Bytes read, possibly fewer than asked on a timeout, or a negated errno
 *   value.
 *
 ****************************************************************************/

ssize_t ny_audio_stream_read(struct ny_audio_stream_s *stream, void *pcm,
                             size_t bytes, int timeout_ms);

/****************************************************************************
 * Name: ny_audio_stream_queued
 *
 * Description:
 *   Playback: bytes written that the driver has not returned yet, which is
 *   the output latency.
 *
 ****************************************************************************/

size_t ny_audio_stream_queued(struct ny_audio_stream_s *stream);

void ny_audio_stream_stats(struct ny_audio_stream_s *stream,
                           struct ny_audio_stream_stats_s *stats);

/****************************************************************************
 * Name: ny_audio_stream_close
 *
 * Description:
 *   Stop, collect the chunks from the driver, release the node.  Safe with
 *   NULL.
 *
 ****************************************************************************/

void ny_audio_stream_close(struct ny_audio_stream_s *stream);

#endif /* __NYABULA_CORE_NY_AUDIO_STREAM_H */
