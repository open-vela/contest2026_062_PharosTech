/****************************************************************************
 * app/camenc/camenc_stream.c
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

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "camenc_stream.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The timescale is microseconds.  It could be anything -- the field exists so
 * that durations and decode times are on a scale the file chooses -- and
 * seconds divided into a million parts is the scale the caller already has,
 * so nothing has to be rounded on the way in or recovered on the way out.
 */

#define CAMENC_TIMESCALE 1000000u

/* Sample duration for the first sample, which has no predecessor to be
 * measured against.  Only the first: the timeline itself comes from the
 * decode times, which are exact, so this affects one fragment's duration
 * field and nothing else.
 */

#define CAMENC_FIRST_DURATION 33333u

/* Sample flags, per ISO/IEC 14496-12 8.8.3.1.
 *
 *   sample_depends_on           bits 6..7    2 = does not depend, 1 = does
 *   sample_is_non_sync_sample   bit 15      0 = a stream may start here
 *
 * A picture that depends on nothing is one a decoder can be started on, so
 * the two flags always move together and are written as a pair.
 */

#define CAMENC_SAMPLE_KEY   0x00000080u
#define CAMENC_SAMPLE_DELTA 0x00008040u

#define CAMENC_NAL_SPS      7u
#define CAMENC_NAL_PPS      8u

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A cursor over the caller's scratch space.  Every write is bounds-checked
 * and latches a failure rather than running off the end; the caller of the
 * builders only has to look at `overflow` once, at the end.
 */

struct bw_s
{
  uint8_t *base;
  size_t size;
  size_t len;
  bool overflow;
};

/****************************************************************************
 * Private Functions: byte output
 ****************************************************************************/

static void bw_u8(struct bw_s *bw, uint8_t v)
{
  if (bw->len + 1 > bw->size)
    {
      bw->overflow = true;
      return;
    }

  bw->base[bw->len++] = v;
}

static void bw_u16(struct bw_s *bw, uint16_t v)
{
  bw_u8(bw, (uint8_t)(v >> 8));
  bw_u8(bw, (uint8_t)v);
}

static void bw_u24(struct bw_s *bw, uint32_t v)
{
  bw_u8(bw, (uint8_t)(v >> 16));
  bw_u8(bw, (uint8_t)(v >> 8));
  bw_u8(bw, (uint8_t)v);
}

static void bw_u32(struct bw_s *bw, uint32_t v)
{
  bw_u8(bw, (uint8_t)(v >> 24));
  bw_u8(bw, (uint8_t)(v >> 16));
  bw_u8(bw, (uint8_t)(v >> 8));
  bw_u8(bw, (uint8_t)v);
}

static void bw_u64(struct bw_s *bw, uint64_t v)
{
  bw_u32(bw, (uint32_t)(v >> 32));
  bw_u32(bw, (uint32_t)v);
}

static void bw_fourcc(struct bw_s *bw, const char *s)
{
  int i;

  for (i = 0; i < 4; i++)
    {
      bw_u8(bw, (uint8_t)s[i]);
    }
}

static void bw_bytes(struct bw_s *bw, const uint8_t *p, size_t n)
{
  if (bw->len + n > bw->size)
    {
      bw->overflow = true;
      return;
    }

  memcpy(bw->base + bw->len, p, n);
  bw->len += n;
}

static void bw_zeros(struct bw_s *bw, size_t n)
{
  if (bw->len + n > bw->size)
    {
      bw->overflow = true;
      return;
    }

  memset(bw->base + bw->len, 0, n);
  bw->len += n;
}

/* The identity transformation matrix, which is all a single video track
 * needs; it is stored as 16.16 except for the last element, which is 2.30.
 */

static void bw_matrix_identity(struct bw_s *bw)
{
  bw_u32(bw, 0x00010000u);
  bw_u32(bw, 0);
  bw_u32(bw, 0);
  bw_u32(bw, 0);
  bw_u32(bw, 0x00010000u);
  bw_u32(bw, 0);
  bw_u32(bw, 0);
  bw_u32(bw, 0);
  bw_u32(bw, 0x40000000u);
}

/****************************************************************************
 * Private Functions: boxes
 *
 * A box is a 32-bit size, a four-character type and a payload.  The size is
 * not known until the payload has been written, so a box is opened by
 * reserving the size field and closed by filling it in.  The offset returned
 * by the open call is what the close needs.
 ****************************************************************************/

static size_t bw_open(struct bw_s *bw, const char *type)
{
  size_t at = bw->len;

  bw_u32(bw, 0); /* patched by bw_close */
  bw_fourcc(bw, type);
  return at;
}

static void bw_close(struct bw_s *bw, size_t at)
{
  size_t size = bw->len - at;
  int i;

  if (bw->overflow || size > 0xffffffffu)
    {
      bw->overflow = true;
      return;
    }

  /* Big-endian, which the file format is throughout, and written by hand
   * because the cursor only goes forwards.
   */

  for (i = 0; i < 4; i++)
    {
      bw->base[at + i] = (uint8_t)(size >> (24 - 8 * i));
    }
}

/* A full box: an ordinary one with a version byte and 24 bits of flags, which
 * is how the format distinguishes revisions of the same box.
 */

static size_t bw_open_full(struct bw_s *bw, const char *type, uint8_t version,
                           uint32_t flags)
{
  size_t at = bw_open(bw, type);

  bw_u8(bw, version);
  bw_u24(bw, flags);
  return at;
}

/****************************************************************************
 * Private Functions: annex-B scanning
 *
 * An access unit is a sequence of NAL units, each introduced by a start code
 * of three or four bytes: 00 00 01, with a fourth zero in front when the
 * writer had one to spare.  Nothing says how long a NAL unit is; it ends
 * where the next start code begins, which is why a decoder cannot skip
 * ahead and why MP4 does not use this framing.
 ****************************************************************************/

static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end)
{
  while (p + 3 <= end)
    {
      if (p[0] == 0 && p[1] == 0 && p[2] == 1)
        {
          return p;
        }

      p++;
    }

  return NULL;
}

/****************************************************************************
 * Private Functions: the initialisation segment
 ****************************************************************************/

static void build_avcc(struct bw_s *bw, const struct camenc_stream_s *st)
{
  size_t at = bw_open(bw, "avcC");

  /* The profile, its constraint flags and the level are the third to fifth
   * bytes of the sequence parameter set.  They are read from it rather than
   * configured separately, so the record cannot come to disagree with the
   * parameter set it describes -- which is what would happen the moment one
   * of the two was updated and the other was not.
   */

  bw_u8(bw, 1);          /* configurationVersion  */
  bw_u8(bw, st->sps[1]); /* AVCProfileIndication  */
  bw_u8(bw, st->sps[2]); /* profile_compatibility */
  bw_u8(bw, st->sps[3]); /* AVCLevelIndication    */

  /* Six bits of reserved ones, then the length of the length prefix, minus
   * one.  Three gives four-byte prefixes, which is what the samples below
   * are written with.
   */

  bw_u8(bw, 0xfcu | 3u);

  bw_u8(bw, 0xe0u | 1u); /* reserved ones, then one SPS */
  bw_u16(bw, (uint16_t)st->sps_len);
  bw_bytes(bw, st->sps, st->sps_len);

  bw_u8(bw, 1u); /* one PPS                    */
  bw_u16(bw, (uint16_t)st->pps_len);
  bw_bytes(bw, st->pps, st->pps_len);

  bw_close(bw, at);
}

static void build_avc1(struct bw_s *bw, const struct camenc_stream_s *st)
{
  size_t at = bw_open(bw, "avc1");

  bw_zeros(bw, 6);  /* reserved                     */
  bw_u16(bw, 1);    /* data_reference_index         */
  bw_u16(bw, 0);    /* predefined                   */
  bw_u16(bw, 0);    /* reserved                     */
  bw_zeros(bw, 12); /* predefined                   */
  bw_u16(bw, (uint16_t)st->width);
  bw_u16(bw, (uint16_t)st->height);
  bw_u32(bw, 0x00480000u); /* 72 dpi, as 16.16             */
  bw_u32(bw, 0x00480000u);
  bw_u32(bw, 0);       /* reserved                     */
  bw_u16(bw, 1);       /* frame_count                  */
  bw_zeros(bw, 32);    /* compressorname, unused       */
  bw_u16(bw, 0x0018u); /* depth: 24-bit colour         */
  bw_u16(bw, 0xffffu); /* predefined: -1               */

  build_avcc(bw, st);

  bw_close(bw, at);
}

static void build_stsd(struct bw_s *bw, const struct camenc_stream_s *st)
{
  size_t at = bw_open_full(bw, "stsd", 0, 0);

  bw_u32(bw, 1); /* entry_count */
  build_avc1(bw, st);

  bw_close(bw, at);
}

/* The sample tables of a fragmented file are empty: every sample is
 * described by the fragment it arrives in, so the initialisation segment
 * declares the shape of the track and nothing about its contents.
 *
 * stsz differs from the rest only in needing a sample_size field before its
 * count, which is why it is not simply the same box with another name.
 */

static void build_empty_table(struct bw_s *bw, const char *type,
                              bool has_sample_size)
{
  size_t at = bw_open_full(bw, type, 0, 0);

  if (has_sample_size)
    {
      bw_u32(bw, 0); /* sample_size  */
    }

  bw_u32(bw, 0); /* entry_count  */
  bw_close(bw, at);
}

static void build_stbl(struct bw_s *bw, const struct camenc_stream_s *st)
{
  size_t at = bw_open(bw, "stbl");

  build_stsd(bw, st);
  build_empty_table(bw, "stts", false);
  build_empty_table(bw, "stsc", false);
  build_empty_table(bw, "stsz", true);
  build_empty_table(bw, "stco", false);

  bw_close(bw, at);
}

static void build_dinf(struct bw_s *bw)
{
  size_t at = bw_open(bw, "dinf");
  size_t dref;
  size_t url;

  dref = bw_open_full(bw, "dref", 0, 0);
  bw_u32(bw, 1); /* entry_count */

  /* A self-contained file: no external data reference.  The flag is what
   * says so, and it is why no URL follows.
   */

  url = bw_open_full(bw, "url ", 0, 1);
  bw_close(bw, url);

  bw_close(bw, dref);
  bw_close(bw, at);
}

static void build_minf(struct bw_s *bw, const struct camenc_stream_s *st)
{
  size_t at = bw_open(bw, "minf");
  size_t vmhd;

  vmhd = bw_open_full(bw, "vmhd", 0, 1);
  bw_u16(bw, 0);   /* graphicsmode */
  bw_zeros(bw, 6); /* opcolor      */
  bw_close(bw, vmhd);

  build_dinf(bw);
  build_stbl(bw, st);

  bw_close(bw, at);
}

static void build_mdia(struct bw_s *bw, const struct camenc_stream_s *st)
{
  size_t at = bw_open(bw, "mdia");
  size_t mdhd;
  size_t hdlr;

  mdhd = bw_open_full(bw, "mdhd", 0, 0);
  bw_u32(bw, 0); /* creation_time   */
  bw_u32(bw, 0); /* modification_time */
  bw_u32(bw, CAMENC_TIMESCALE);
  bw_u32(bw, 0);       /* duration: unknown, a live stream */
  bw_u16(bw, 0x55c4u); /* language: "und" */
  bw_u16(bw, 0);       /* predefined      */
  bw_close(bw, mdhd);

  hdlr = bw_open_full(bw, "hdlr", 0, 0);
  bw_u32(bw, 0);         /* predefined      */
  bw_fourcc(bw, "vide"); /* handler_type    */
  bw_zeros(bw, 12);      /* reserved        */
  bw_bytes(bw, (const uint8_t *)"VideoHandler", 13);
  bw_close(bw, hdlr);

  build_minf(bw, st);

  bw_close(bw, at);
}

static void build_trak(struct bw_s *bw, const struct camenc_stream_s *st)
{
  size_t at = bw_open(bw, "trak");
  size_t tkhd;

  tkhd = bw_open_full(bw, "tkhd", 0, 3); /* enabled | in movie */
  bw_u32(bw, 0);                         /* creation_time        */
  bw_u32(bw, 0);                         /* modification_time    */
  bw_u32(bw, 1);                         /* track_ID             */
  bw_u32(bw, 0);                         /* reserved             */
  bw_u32(bw, 0);                         /* duration: live       */
  bw_zeros(bw, 8);                       /* reserved             */
  bw_u16(bw, 0);                         /* layer                */
  bw_u16(bw, 0);                         /* alternate_group      */
  bw_u16(bw, 0);                         /* volume: not audio    */
  bw_u16(bw, 0);                         /* reserved             */
  bw_matrix_identity(bw);
  bw_u32(bw, st->width << 16); /* 16.16 fixed point    */
  bw_u32(bw, st->height << 16);
  bw_close(bw, tkhd);

  build_mdia(bw, st);

  bw_close(bw, at);
}

/* trex gives the defaults every fragment's samples inherit, which is what
 * tells a reader this is a fragmented file rather than one whose sample
 * tables were lost.
 */

static void build_mvex(struct bw_s *bw)
{
  size_t at = bw_open(bw, "mvex");
  size_t trex;

  trex = bw_open_full(bw, "trex", 0, 0);
  bw_u32(bw, 1); /* track_ID                          */
  bw_u32(bw, 1); /* default_sample_description_index  */
  bw_u32(bw, 0); /* default_sample_duration           */
  bw_u32(bw, 0); /* default_sample_size               */
  bw_u32(bw, 0); /* default_sample_flags              */
  bw_close(bw, trex);

  bw_close(bw, at);
}

static void build_moov(struct bw_s *bw, const struct camenc_stream_s *st)
{
  size_t at = bw_open(bw, "moov");
  size_t mvhd;

  mvhd = bw_open_full(bw, "mvhd", 0, 0);
  bw_u32(bw, 0); /* creation_time           */
  bw_u32(bw, 0); /* modification_time       */
  bw_u32(bw, CAMENC_TIMESCALE);
  bw_u32(bw, 0);           /* duration: live          */
  bw_u32(bw, 0x00010000u); /* rate: 1.0               */
  bw_u16(bw, 0x0100u);     /* volume: 1.0             */
  bw_u16(bw, 0);           /* reserved                */
  bw_zeros(bw, 8);         /* reserved                */
  bw_matrix_identity(bw);
  bw_zeros(bw, 24); /* predefined              */
  bw_u32(bw, 2);    /* next_track_ID           */
  bw_close(bw, mvhd);

  build_trak(bw, st);
  build_mvex(bw);

  bw_close(bw, at);
}

static void build_ftyp(struct bw_s *bw)
{
  size_t at = bw_open(bw, "ftyp");

  bw_fourcc(bw, "iso5"); /* major_brand     */
  bw_u32(bw, 512);       /* minor_version   */

  /* Compatible brands.  iso5 is what a fragmented file is, and the rest say
   * what may be found inside it; naming them costs sixteen bytes and saves a
   * reader from inferring the contents from the boxes.
   */

  bw_fourcc(bw, "iso5");
  bw_fourcc(bw, "iso6");
  bw_fourcc(bw, "mp41");
  bw_fourcc(bw, "dash");
  bw_fourcc(bw, "avc1");

  bw_close(bw, at);
}

static int emit_init(struct camenc_stream_s *st)
{
  struct bw_s bw;
  int ret;

  if (!st->params_known)
    {
      return -EINVAL;
    }

  bw.base = st->scratch;
  bw.size = st->scratch_size;
  bw.len = 0;
  bw.overflow = false;

  build_ftyp(&bw);
  build_moov(&bw, st);

  if (bw.overflow)
    {
      return -ENOSPC;
    }

  ret = st->emit(st->arg, CAMENC_SEG_INIT, st->scratch, bw.len, true);
  if (ret < 0)
    {
      return ret;
    }

  st->init_sent = true;
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int camenc_stream_init(struct camenc_stream_s *st, uint32_t width,
                       uint32_t height, uint32_t max_au, uint8_t *scratch,
                       size_t scratch_size, camenc_emit_fn emit, void *arg)
{
  if (st == NULL || scratch == NULL || emit == NULL)
    {
      return -EINVAL;
    }

  if (width == 0 || height == 0 || max_au == 0)
    {
      return -EINVAL;
    }

  if (scratch_size < (size_t)max_au + CAMENC_STREAM_SLACK)
    {
      return -EINVAL;
    }

  memset(st, 0, sizeof(*st));

  st->emit = emit;
  st->arg = arg;
  st->width = width;
  st->height = height;
  st->max_au = max_au;
  st->scratch = scratch;
  st->scratch_size = scratch_size;

  return 0;
}

int camenc_stream_repeat_init(struct camenc_stream_s *st)
{
  if (st == NULL || !st->params_known)
    {
      return -EINVAL;
    }

  return emit_init(st);
}

size_t camenc_stream_codec_string(FAR const struct camenc_stream_s *st,
                                  FAR char *buf, size_t len)
{
  int n;

  if (st == NULL || buf == NULL || !st->params_known || st->sps_len < 4)
    {
      return 0;
    }

  /* The profile, its constraint flags and the level are the second, third
   * and fourth bytes of the parameter set: the first is the NAL header.
   */

  n = snprintf(buf, len, "avc1.%02x%02x%02x", st->sps[1], st->sps[2],
               st->sps[3]);

  return (n > 0 && (size_t)n < len) ? (size_t)n : 0;
}

int camenc_stream_write(struct camenc_stream_s *st, const uint8_t *au,
                        size_t len, uint64_t pts_us, bool key)
{
  const uint8_t *au_end;
  const uint8_t *sc;
  struct bw_s bw;
  size_t moof_at;
  size_t traf_at;
  size_t trun_at;
  size_t trun_samples_at;
  size_t mdat_at;
  uint32_t sample_size;
  uint32_t duration;
  int ret;

  if (st == NULL || au == NULL)
    {
      return -EINVAL;
    }

  if (len == 0 || len > st->max_au)
    {
      return -ENOSPC;
    }

  au_end = au + len;

  /* First pass.  The sample's size and its NAL count go into the fragment's
   * header, so they have to be known before the header is written -- and
   * they are what the second pass arrives at, rather than being recounted.
   *
   * Parameter sets are picked out here rather than copied into the sample:
   * MP4 keeps them in the initialisation segment, and leaving them in the
   * samples as well would be describing the same thing twice.
   */

  sample_size = 0;

  sc = find_start_code(au, au_end);
  if (sc == NULL)
    {
      return -EINVAL;
    }

  while (sc != NULL)
    {
      const uint8_t *nal = sc + 3;
      const uint8_t *next = find_start_code(nal, au_end);
      const uint8_t *nal_end = next != NULL ? next : au_end;
      size_t nal_len;
      uint8_t type;

      /* Trailing zeros are not part of the unit.  A four-byte start code is
       * a three-byte one with a zero in front, and this is where that zero
       * is given up.
       */

      while (nal_end > nal && nal_end[-1] == 0)
        {
          nal_end--;
        }

      nal_len = (size_t)(nal_end - nal);
      if (nal_len == 0)
        {
          sc = next;
          continue;
        }

      type = (uint8_t)(nal[0] & 0x1fu);

      if (type == CAMENC_NAL_SPS || type == CAMENC_NAL_PPS)
        {
          uint8_t *dst = type == CAMENC_NAL_SPS ? st->sps : st->pps;
          size_t cap =
              type == CAMENC_NAL_SPS ? CAMENC_SPS_MAX : CAMENC_PPS_MAX;

          if (nal_len <= cap)
            {
              memcpy(dst, nal, nal_len);

              if (type == CAMENC_NAL_SPS)
                {
                  st->sps_len = nal_len;
                }
              else
                {
                  st->pps_len = nal_len;
                }
            }
        }
      else
        {
          sample_size += 4u + (uint32_t)nal_len;
        }

      sc = next;
    }

  if (st->sps_len == 0 || st->pps_len == 0)
    {
      return -EINVAL;
    }

  st->params_known = true;

  if (!st->init_sent)
    {
      ret = emit_init(st);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (sample_size == 0)
    {
      /* Only parameter sets: there is no picture to put in a fragment.  The
       * parameters were still worth having, so this is not an error.
       */

      return 0;
    }

  if ((size_t)sample_size + CAMENC_STREAM_SLACK > st->scratch_size)
    {
      return -ENOSPC;
    }

  /* The duration of this sample is the gap from the previous one.  The first
   * has nothing to measure against and takes the nominal figure, which
   * affects that one fragment's duration field and not the timeline: the
   * timeline is the decode times, and those are the ones the caller
   * measured.
   */

  if (st->have_last_pts && pts_us > st->last_pts)
    {
      duration = (uint32_t)(pts_us - st->last_pts);
    }
  else
    {
      duration = CAMENC_FIRST_DURATION;
    }

  st->last_pts = pts_us;
  st->have_last_pts = true;

  bw.base = st->scratch;
  bw.size = st->scratch_size;
  bw.len = 0;
  bw.overflow = false;

  moof_at = bw_open(&bw, "moof");

  {
    size_t mfhd = bw_open_full(&bw, "mfhd", 0, 0);
    bw_u32(&bw, st->seq);
    bw_close(&bw, mfhd);
  }

  traf_at = bw_open(&bw, "traf");

  {
    /* default-base-is-moof: the offsets below are measured from the start of
     * this fragment's moof box, so the fragment carries its own addressing
     * and can be placed anywhere in a stream -- which is the whole point of
     * fragmenting it.
     */

    size_t tfhd = bw_open_full(&bw, "tfhd", 0, 0x020000u);
    bw_u32(&bw, 1); /* track_ID */
    bw_close(&bw, tfhd);
  }

  {
    size_t tfdt = bw_open_full(&bw, "tfdt", 1, 0);
    bw_u64(&bw, pts_us);
    bw_close(&bw, tfdt);
  }

  /* trun.  There is one sample per fragment -- an access unit is a sample,
   * however many units it is made of -- so its sample table is one row: a
   * duration, a size and flags.
   *
   * Its data offset cannot be known until the moof is complete, so the box
   * is written with a placeholder and the offset filled in below.  That is
   * why the offset's position is remembered.
   */

  trun_at = bw_open_full(&bw, "trun", 0,
                         0x000001u |     /* data-offset-present       */
                             0x000100u | /* sample-duration-present   */
                             0x000200u | /* sample-size-present       */
                             0x000400u); /* sample-flags-present      */
  bw_u32(&bw, 1);                        /* sample_count              */
  trun_samples_at = bw.len;
  bw_u32(&bw, 0); /* data_offset, patched below */

  bw_u32(&bw, duration);
  bw_u32(&bw, sample_size);
  bw_u32(&bw, key ? CAMENC_SAMPLE_KEY : CAMENC_SAMPLE_DELTA);

  bw_close(&bw, trun_at);
  bw_close(&bw, traf_at);
  bw_close(&bw, moof_at);

  mdat_at = bw_open(&bw, "mdat");

  /* Second pass: the sample itself, each unit prefixed with its length. */

  sc = find_start_code(au, au_end);
  while (sc != NULL)
    {
      const uint8_t *nal = sc + 3;
      const uint8_t *next = find_start_code(nal, au_end);
      const uint8_t *nal_end = next != NULL ? next : au_end;
      size_t nal_len;
      uint8_t type;

      while (nal_end > nal && nal_end[-1] == 0)
        {
          nal_end--;
        }

      nal_len = (size_t)(nal_end - nal);
      if (nal_len > 0)
        {
          type = (uint8_t)(nal[0] & 0x1fu);

          if (type != CAMENC_NAL_SPS && type != CAMENC_NAL_PPS)
            {
              bw_u32(&bw, (uint32_t)nal_len);
              bw_bytes(&bw, nal, nal_len);
            }
        }

      sc = next;
    }

  bw_close(&bw, mdat_at);

  if (bw.overflow)
    {
      return -ENOSPC;
    }

  /* The data offset is the distance from the start of the moof to the first
   * byte of the sample, which is the moof's size plus the mdat header.  It
   * could only be written once the moof was complete.
   */

  {
    uint32_t offset = (uint32_t)mdat_at + 8u;
    int i;

    for (i = 0; i < 4; i++)
      {
        bw.base[trun_samples_at + i] = (uint8_t)(offset >> (24 - 8 * i));
      }
  }

  ret = st->emit(st->arg, CAMENC_SEG_FRAGMENT, st->scratch, bw.len, key);
  if (ret < 0)
    {
      return ret;
    }

  st->seq++;
  return 0;
}
