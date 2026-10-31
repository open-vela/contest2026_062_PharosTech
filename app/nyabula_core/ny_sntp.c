/****************************************************************************
 * app/nyabula_core/ny_sntp.c
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

#include "ny_sntp.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

/* Byte offsets in the NTP header. */

#define NY_SNTP_AT_FLAGS    0 /* LI:2 | VN:3 | Mode:3 */
#define NY_SNTP_AT_STRATUM  1
#define NY_SNTP_AT_REFID    12
#define NY_SNTP_AT_ORIGIN   24
#define NY_SNTP_AT_RECEIVE  32
#define NY_SNTP_AT_TRANSMIT 40

#define NY_SNTP_VERSION     4
#define NY_SNTP_MODE_CLIENT 3
#define NY_SNTP_MODE_SERVER 4
#define NY_SNTP_LEAP_ALARM  3 /* "clock not synchronised" */
#define NY_SNTP_STRATUM_MAX 15

/* Seconds from the NTP epoch (1900) to the Unix one (1970), and the length
 * of one NTP era, after which the 32-bit seconds field starts again.
 */

#define NY_SNTP_UNIX_OFFSET 2208988800LL
#define NY_SNTP_ERA_SECONDS 4294967296LL

static uint32_t ny_sntp_get32(const uint8_t *at);
static int64_t ny_sntp_stamp_ms(const uint8_t *at);
static bool ny_sntp_separator(char c);

/****************************************************************************
 * Name: ny_sntp_get32
 ****************************************************************************/

static uint32_t ny_sntp_get32(const uint8_t *at)
{
  return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) |
         ((uint32_t)at[2] << 8) | (uint32_t)at[3];
}

/****************************************************************************
 * Name: ny_sntp_stamp_ms
 *
 * Description:
 *   A 64-bit NTP timestamp as Unix milliseconds.  The seconds field wraps
 *   in February 2036.  A value with the top bit set belongs to the era that
 *   began in 1900 -- it covers 1968 to 2036 -- and one without it to the era
 *   after, which keeps this correct until 2104 instead of turning 2036 into
 *   1900.
 *
 ****************************************************************************/

static int64_t ny_sntp_stamp_ms(const uint8_t *at)
{
  uint32_t seconds = ny_sntp_get32(at);
  uint32_t fraction = ny_sntp_get32(at + 4);
  int64_t unix_seconds = (int64_t)seconds - NY_SNTP_UNIX_OFFSET;
  if ((seconds & 0x80000000u) == 0)
    {
      unix_seconds += NY_SNTP_ERA_SECONDS;
    }

  return unix_seconds * 1000 + (int64_t)(((uint64_t)fraction * 1000) >> 32);
}

/****************************************************************************
 * Name: ny_sntp_separator
 ****************************************************************************/

static bool ny_sntp_separator(char c)
{
  return c == ';' || c == ',' || c == ' ' || c == '\t';
}

/****************************************************************************
 * Name: ny_sntp_build
 ****************************************************************************/

void ny_sntp_build(uint8_t packet[NY_SNTP_PACKET_SIZE], uint64_t token)
{
  memset(packet, 0, NY_SNTP_PACKET_SIZE);
  packet[NY_SNTP_AT_FLAGS] = (NY_SNTP_VERSION << 3) | NY_SNTP_MODE_CLIENT;

  /* Everything else stays zero.  A client that has no idea what time it is
   * has nothing truthful to put into the other fields, and the transmit
   * timestamp is only ever echoed back, so it carries the token rather than
   * a clock reading that would be wrong anyway.
   */

  for (int i = 0; i < 8; i++)
    {
      packet[NY_SNTP_AT_TRANSMIT + i] = (uint8_t)(token >> (56 - 8 * i));
    }
}

/****************************************************************************
 * Name: ny_sntp_parse
 ****************************************************************************/

int ny_sntp_parse(const uint8_t *packet, size_t length, uint64_t token,
                  int64_t floor_ms, int64_t ceiling_ms,
                  struct ny_sntp_reply_s *reply)
{
  uint64_t origin = 0;
  unsigned int version;
  memset(reply, 0, sizeof(*reply));
  if (token == 0)
    {
      return -EINVAL;
    }

  if (packet == NULL || length < NY_SNTP_PACKET_SIZE)
    {
      return -EMSGSIZE;
    }

  version = (packet[NY_SNTP_AT_FLAGS] >> 3) & 7;
  if ((packet[NY_SNTP_AT_FLAGS] & 7) != NY_SNTP_MODE_SERVER ||
      (version != 3 && version != NY_SNTP_VERSION))
    {
      return -EPROTO;
    }

  /* Before anything the packet says is believed: a reply that does not
   * echo the token was not caused by this request, whatever else is in it.
   */

  for (int i = 0; i < 8; i++)
    {
      origin = (origin << 8) | packet[NY_SNTP_AT_ORIGIN + i];
    }

  if (origin != token)
    {
      return -ESTALE;
    }

  reply->leap = packet[NY_SNTP_AT_FLAGS] >> 6;
  reply->stratum = packet[NY_SNTP_AT_STRATUM];
  if (reply->stratum == 0)
    {
      /* A kiss-o'-death: the reference identifier is four ASCII letters
       * saying why the server will not answer (RATE, DENY, RSTR ...).
       */

      for (int i = 0; i < 4; i++)
        {
          char c = (char)packet[NY_SNTP_AT_REFID + i];
          reply->kiss[i] = c >= 0x20 && c < 0x7f ? c : '?';
        }

      return -ECONNREFUSED;
    }

  if (reply->leap == NY_SNTP_LEAP_ALARM ||
      reply->stratum > NY_SNTP_STRATUM_MAX)
    {
      return -EHOSTUNREACH;
    }

  if (ny_sntp_get32(packet + NY_SNTP_AT_TRANSMIT) == 0 &&
      ny_sntp_get32(packet + NY_SNTP_AT_TRANSMIT + 4) == 0)
    {
      return -EINVAL;
    }

  reply->receive_ms = ny_sntp_stamp_ms(packet + NY_SNTP_AT_RECEIVE);
  reply->transmit_ms = ny_sntp_stamp_ms(packet + NY_SNTP_AT_TRANSMIT);
  if (reply->transmit_ms < floor_ms || reply->transmit_ms > ceiling_ms)
    {
      return -ERANGE;
    }

  return 0;
}

/****************************************************************************
 * Name: ny_sntp_now_ms
 ****************************************************************************/

int64_t ny_sntp_now_ms(const struct ny_sntp_reply_s *reply,
                       uint64_t roundtrip_ms)
{
  /* The reply left the server at transmit_ms and then spent half of the
   * time on the network getting here.  The time on the network is the
   * round trip less what the server spent between receiving and sending; a
   * server that reports nonsense for that is simply not credited with any.
   */

  int64_t held = reply->transmit_ms - reply->receive_ms;
  int64_t travel = (int64_t)roundtrip_ms;
  if (held > 0 && held <= travel)
    {
      travel -= held;
    }

  return reply->transmit_ms + travel / 2;
}

/****************************************************************************
 * Name: ny_sntp_date_ms
 ****************************************************************************/

int64_t ny_sntp_date_ms(const char *date)
{
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int month = 0;
  int day = 0;
  int year = 0;
  int64_t era;
  int64_t year_of_era;
  int64_t day_of_year;
  int64_t day_of_era;

  /* "Mmm dd yyyy", the day padded with a blank below ten. */

  if (date == NULL || strlen(date) != 11 || date[3] != ' ' || date[6] != ' ')
    {
      return -1;
    }

  while (month < 12 && strncmp(date, months + month * 3, 3) != 0)
    {
      month++;
    }

  if (month == 12 || (date[4] != ' ' && (date[4] < '0' || date[4] > '3')) ||
      date[5] < '0' || date[5] > '9')
    {
      return -1;
    }

  day = (date[4] == ' ' ? 0 : date[4] - '0') * 10 + (date[5] - '0');
  for (int i = 7; i < 11; i++)
    {
      if (date[i] < '0' || date[i] > '9')
        {
          return -1;
        }

      year = year * 10 + (date[i] - '0');
    }

  if (day < 1 || day > 31 || year < 1970)
    {
      return -1;
    }

  /* Days since 1970-01-01 in the proleptic Gregorian calendar, counted in
   * years that begin in March so that the leap day is the last one.
   */

  month += 1;
  year -= month <= 2;
  era = year / 400;
  year_of_era = year - era * 400;
  day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return (era * 146097 + day_of_era - 719468) * 86400000LL;
}

/****************************************************************************
 * Name: ny_sntp_server
 ****************************************************************************/

int ny_sntp_server(const char *list, unsigned int index, char *out,
                   size_t size)
{
  const char *at = list;
  if (list == NULL || out == NULL || size == 0)
    {
      return -EINVAL;
    }

  for (;;)
    {
      size_t length = 0;
      while (ny_sntp_separator(*at))
        {
          at++;
        }

      if (*at == '\0')
        {
          return -ENOENT;
        }

      while (at[length] != '\0' && !ny_sntp_separator(at[length]))
        {
          length++;
        }

      if (index == 0)
        {
          if (length >= size)
            {
              return -ENAMETOOLONG;
            }

          memcpy(out, at, length);
          out[length] = '\0';
          return 0;
        }

      index--;
      at += length;
    }
}
